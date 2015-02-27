/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file Host.cpp
 * @authors:
 *   Gav Wood <i@gavwood.com>
 *   Eric Lombrozo <elombrozo@gmail.com> (Windows version of populateAddresses())
 * @date 2014
 */

#include "Host.h"

#include <sys/types.h>
#ifdef _WIN32
// winsock is already included
// #include <winsock.h>
#else
#include <ifaddrs.h>
#endif

#include <set>
#include <chrono>
#include <thread>
#include <boost/algorithm/string.hpp>
#include <libdevcore/Common.h>
#include <libethcore/Exceptions.h>
#include "Session.h"
#include "Capability.h"
#include "UPnP.h"
using namespace std;
using namespace dev;
using namespace dev::p2p;

// Addresses we will skip during network interface discovery
// Use a vector as the list is small
// Why this and not names?
// Under MacOSX loopback (127.0.0.1) can be named lo0 and br0 are bridges (0.0.0.0)
static const set<bi::address> c_rejectAddresses = {
	{bi::address_v4::from_string("127.0.0.1")},
	{bi::address_v6::from_string("::1")},
	{bi::address_v4::from_string("0.0.0.0")},
	{bi::address_v6::from_string("::")}
};

Host::Host(std::string const& _clientVersion, NetworkPreferences const& _n, bool _start):
	Worker("p2p"),
	m_clientVersion(_clientVersion),
	m_netPrefs(_n),
	m_acceptor(m_ioService),
	m_socket(m_ioService),
	m_id(h512::random())
{
	populateAddresses();
	m_lastPeersRequest = chrono::steady_clock::time_point::min();
	clog(NetNote) << "Id:" << m_id.abridged();
	if (_start)
		start();
}

Host::~Host()
{
	stop();
}

void Host::start()
{
	if (isWorking())
		stop();

	for (unsigned i = 0; i < 2; ++i)
	{
		bi::tcp::endpoint endpoint(bi::tcp::v4(), i ? 0 : m_netPrefs.listenPort);
		try
		{
			m_acceptor.open(endpoint.protocol());
			m_acceptor.set_option(ba::socket_base::reuse_address(true));
			m_acceptor.bind(endpoint);
			m_acceptor.listen();
			m_listenPort = i ? m_acceptor.local_endpoint().port() : m_netPrefs.listenPort;
			break;
		}
		catch (...)
		{
			if (i)
			{
				cwarn << "Couldn't start accepting connections on host. Something very wrong with network?";
				return;
			}
			m_acceptor.close();
			continue;
		}
	}

	determinePublic(m_netPrefs.publicIP, m_netPrefs.upnp);
	ensureAccepting();

	m_incomingPeers.clear();
	m_freePeers.clear();

	m_lastPeersRequest = chrono::steady_clock::time_point::min();
	clog(NetNote) << "Id:" << m_id.abridged();

	for (auto const& h: m_capabilities)
		h.second->onStarting();

	startWorking();
}

void Host::stop()
{
	for (auto const& h: m_capabilities)
		h.second->onStopping();

	stopWorking();

	if (m_acceptor.is_open())
	{
		if (m_accepting)
			m_acceptor.cancel();
		m_acceptor.close();
		m_accepting = false;
	}
	if (m_socket.is_open())
		m_socket.close();
	disconnectPeers();

	m_ioService.reset();
}

unsigned Host::protocolVersion() const
{
	return 0;
}

void Host::registerPeer(std::shared_ptr<Session> _s, vector<string> const& _caps)
{
	{
		Guard l(x_peers);
		m_peers[_s->m_id] = _s;
	}
	for (auto const& i: _caps)
		if (haveCapability(i))
			_s->m_capabilities[i] = shared_ptr<Capability>(m_capabilities[i]->newPeerCapability(_s.get()));
}

void Host::disconnectPeers()
{
	for (unsigned n = 0;; n = 0)
	{
		{
			Guard l(x_peers);
			for (auto i: m_peers)
				if (auto p = i.second.lock())
				{
					p->disconnect(ClientQuit);
					n++;
				}
		}
		if (!n)
			break;
		m_ioService.poll();
		this_thread::sleep_for(chrono::milliseconds(100));
	}

	delete m_upnp;
}

void Host::seal(bytes& _b)
{
	_b[0] = 0x22;
	_b[1] = 0x40;
	_b[2] = 0x08;
	_b[3] = 0x91;
	uint32_t len = (uint32_t)_b.size() - 8;
	_b[4] = (len >> 24) & 0xff;
	_b[5] = (len >> 16) & 0xff;
	_b[6] = (len >> 8) & 0xff;
	_b[7] = len & 0xff;
}

void Host::determinePublic(string const& _publicAddress, bool _upnp)
{
	if (_upnp)
		try
		{
			m_upnp = new UPnP;
		}
		catch (NoUPnPDevice) {}	// let m_upnp continue as null - we handle it properly.

	bi::tcp::resolver r(m_ioService);
	if (m_upnp && m_upnp->isValid() && m_peerAddresses.size())
	{
		clog(NetNote) << "External addr:" << m_upnp->externalIP();
		int p = m_upnp->addRedirect(m_peerAddresses[0].to_string().c_str(), m_listenPort);
		if (p)
			clog(NetNote) << "Punched through NAT and mapped local port" << m_listenPort << "onto external port" << p << ".";
		else
		{
			// couldn't map
			clog(NetWarn) << "Couldn't punch through NAT (or no NAT in place). Assuming" << m_listenPort << "is local & external port.";
			p = m_listenPort;
		}

		auto eip = m_upnp->externalIP();
		if (eip == string("0.0.0.0") && _publicAddress.empty())
			m_public = bi::tcp::endpoint(bi::address(), (unsigned short)p);
		else
		{
			m_public = bi::tcp::endpoint(bi::address::from_string(_publicAddress.empty() ? eip : _publicAddress), (unsigned short)p);
			m_addresses.push_back(m_public.address().to_v4());
		}
	}
	else
	{
		// No UPnP - fallback on given public address or, if empty, the assumed peer address.
		m_public = bi::tcp::endpoint(_publicAddress.size() ? bi::address::from_string(_publicAddress)
									: m_peerAddresses.size() ? m_peerAddresses[0]
									: bi::address(), m_listenPort);
		m_addresses.push_back(m_public.address().to_v4());
	}
}

void Host::populateAddresses()
{
#ifdef _WIN32
	WSAData wsaData;
	if (WSAStartup(MAKEWORD(1, 1), &wsaData) != 0)
		throw NoNetworking();

	char ac[80];
	if (gethostname(ac, sizeof(ac)) == SOCKET_ERROR)
	{
		clog(NetWarn) << "Error " << WSAGetLastError() << " when getting local host name.";
		WSACleanup();
		throw NoNetworking();
	}

	struct hostent* phe = gethostbyname(ac);
	if (phe == 0)
	{
		clog(NetWarn) << "Bad host lookup.";
		WSACleanup();
		throw NoNetworking();
	}

	for (int i = 0; phe->h_addr_list[i] != 0; ++i)
	{
		struct in_addr addr;
		memcpy(&addr, phe->h_addr_list[i], sizeof(struct in_addr));
		char *addrStr = inet_ntoa(addr);
		bi::address ad(bi::address::from_string(addrStr));
		m_addresses.push_back(ad.to_v4());
		bool isLocal = std::find(c_rejectAddresses.begin(), c_rejectAddresses.end(), ad) != c_rejectAddresses.end();
		if (!isLocal)
			m_peerAddresses.push_back(ad.to_v4());
		clog(NetNote) << "Address: " << ac << " = " << m_addresses.back() << (isLocal ? " [LOCAL]" : " [PEER]");
	}

	WSACleanup();
#else
	ifaddrs* ifaddr;
	if (getifaddrs(&ifaddr) == -1)
		throw NoNetworking();

	bi::tcp::resolver r(m_ioService);

	for (ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next)
	{
		if (!ifa->ifa_addr)
			continue;
		if (ifa->ifa_addr->sa_family == AF_INET)
		{
			char host[NI_MAXHOST];
			if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST))
				continue;
			try
			{
				auto it = r.resolve({host, "30303"});
				bi::tcp::endpoint ep = it->endpoint();
				bi::address ad = ep.address();
				m_addresses.push_back(ad.to_v4());
				bool isLocal = std::find(c_rejectAddresses.begin(), c_rejectAddresses.end(), ad) != c_rejectAddresses.end();
				if (!isLocal)
					m_peerAddresses.push_back(ad.to_v4());
				clog(NetNote) << "Address: " << host << " = " << m_addresses.back() << (isLocal ? " [LOCAL]" : " [PEER]");
			}
			catch (...)
			{
				clog(NetNote) << "Couldn't resolve: " << host;
			}
		}
	}

	freeifaddrs(ifaddr);
#endif
}

std::map<h512, bi::tcp::endpoint> Host::potentialPeers()
{
	std::map<h512, bi::tcp::endpoint> ret;
	if (!m_public.address().is_unspecified())
		ret.insert(make_pair(m_id, m_public));
	Guard l(x_peers);
	for (auto i: m_peers)
		if (auto j = i.second.lock())
		{
			auto ep = j->endpoint();
//			cnote << "Checking potential peer" << j->m_listenPort << j->endpoint() << isPrivateAddress(ep.address()) << ep.port() << j->m_id.abridged();
			// Skip peers with a listen port of zero or are on a private network
			bool peerOnNet = (j->m_listenPort != 0 && (!isPrivateAddress(ep.address()) || m_netPrefs.localNetworking));
			if (!peerOnNet && m_incomingPeers.count(j->m_id))
			{
				ep = m_incomingPeers.at(j->m_id).first;
				peerOnNet = (j->m_listenPort != 0 && (!isPrivateAddress(ep.address()) || m_netPrefs.localNetworking));
			}
			if (peerOnNet && ep.port() && j->m_id)
				ret.insert(make_pair(i.first, ep));
		}
	return ret;
}

void Host::ensureAccepting()
{
	if (!m_accepting)
	{
		clog(NetConnect) << "Listening on local port " << m_listenPort << " (public: " << m_public << ")";
		m_accepting = true;
		m_acceptor.async_accept(m_socket, [=](boost::system::error_code ec)
		{
			if (!ec)
				try
				{
					try {
						clog(NetConnect) << "Accepted connection from " << m_socket.remote_endpoint();
					} catch (...){}
					bi::address remoteAddress = m_socket.remote_endpoint().address();
					// Port defaults to 0 - we let the hello tell us which port the peer listens to
					auto p = std::make_shared<Session>(this, std::move(m_socket), remoteAddress);
					p->start();
				}
				catch (std::exception const& _e)
				{
					clog(NetWarn) << "ERROR: " << _e.what();
				}
			m_accepting = false;
			if (ec.value() < 1)
				ensureAccepting();
		});
	}
}

string Host::pocHost()
{
	vector<string> strs;
	boost::split(strs, dev::Version, boost::is_any_of("."));
	return "poc-" + strs[1] + ".ethdev.com";
}

void Host::connect(std::string const& _addr, unsigned short _port) noexcept
{
	for (int i = 0; i < 2; ++i)
		try
		{
			if (i == 0)
			{
				bi::tcp::resolver r(m_ioService);
				connect(r.resolve({_addr, toString(_port)})->endpoint());
			}
			else
				connect(bi::tcp::endpoint(bi::address::from_string(_addr), _port));
			break;
		}
		catch (exception const& e)
		{
			// Couldn't connect
			clog(NetConnect) << "Bad host " << _addr << " (" << e.what() << ")";
		}
}

void Host::connect(bi::tcp::endpoint const& _ep)
{
	clog(NetConnect) << "Attempting connection to " << _ep;
	bi::tcp::socket* s = new bi::tcp::socket(m_ioService);
	s->async_connect(_ep, [=](boost::system::error_code const& ec)
	{
		if (ec)
		{
			clog(NetConnect) << "Connection refused to " << _ep << " (" << ec.message() << ")";
			for (auto i = m_incomingPeers.begin(); i != m_incomingPeers.end(); ++i)
				if (i->second.first == _ep && i->second.second < 3)
				{
					m_freePeers.push_back(i->first);
					goto OK;
				}
			// for-else
			clog(NetConnect) << "Giving up.";
			OK:;
		}
		else
		{
			auto p = make_shared<Session>(this, std::move(*s), _ep.address(), _ep.port());
			clog(NetConnect) << "Connected to " << _ep;
			p->start();
		}
		delete s;
	});
}

bool Host::havePeer(h512 _id) const
{
	Guard l(x_peers);

	// Remove dead peers from list.
	for (auto i = m_peers.begin(); i != m_peers.end();)
		if (i->second.lock().get())
			++i;
		else
			i = m_peers.erase(i);

	return !!m_peers.count(_id);
}

void Host::growPeers()
{
	Guard l(x_peers);
	while (m_peers.size() < m_idealPeerCount)
	{
		if (m_freePeers.empty())
		{
			if (chrono::steady_clock::now() > m_lastPeersRequest + chrono::seconds(10))
			{
				RLPStream s;
				bytes b;
				(Session::prep(s).appendList(1) << GetPeersPacket).swapOut(b);
				seal(b);
				for (auto const& i: m_peers)
					if (auto p = i.second.lock())
						if (p->isOpen())
							p->send(&b);
				m_lastPeersRequest = chrono::steady_clock::now();
			}

			if (!m_accepting)
				ensureAccepting();

			break;
		}

		auto x = time(0) % m_freePeers.size();
		m_incomingPeers[m_freePeers[x]].second++;
		if (!m_peers.count(m_freePeers[x]))
			connect(m_incomingPeers[m_freePeers[x]].first);
		m_freePeers.erase(m_freePeers.begin() + x);
	}
}

void Host::prunePeers()
{
	Guard l(x_peers);
	// We'll keep at most twice as many as is ideal, halfing what counts as "too young to kill" until we get there.
	for (unsigned old = 15000; m_peers.size() > m_idealPeerCount * 2 && old > 100; old /= 2)
		while (m_peers.size() > m_idealPeerCount)
		{
			// look for worst peer to kick off
			// first work out how many are old enough to kick off.
			shared_ptr<Session> worst;
			unsigned agedPeers = 0;
			for (auto i: m_peers)
				if (auto p = i.second.lock())
					if (/*(m_mode != NodeMode::Host || p->m_caps != 0x01) &&*/ chrono::steady_clock::now() > p->m_connect + chrono::milliseconds(old))	// don't throw off new peers; peer-servers should never kick off other peer-servers.
					{
						++agedPeers;
						if ((!worst || p->m_rating < worst->m_rating || (p->m_rating == worst->m_rating && p->m_connect > worst->m_connect)))	// kill older ones
							worst = p;
					}
			if (!worst || agedPeers <= m_idealPeerCount)
				break;
			worst->disconnect(TooManyPeers);
		}

	// Remove dead peers from list.
	for (auto i = m_peers.begin(); i != m_peers.end();)
		if (i->second.lock().get())
			++i;
		else
			i = m_peers.erase(i);
}

std::vector<PeerInfo> Host::peers(bool _updatePing) const
{
	Guard l(x_peers);
    if (_updatePing)
		const_cast<Host*>(this)->pingAll();
	this_thread::sleep_for(chrono::milliseconds(200));
	std::vector<PeerInfo> ret;
	for (auto& i: m_peers)
		if (auto j = i.second.lock())
			if (j->m_socket.is_open())
				ret.push_back(j->m_info);
	return ret;
}

void Host::doWork()
{
	growPeers();
	prunePeers();
	m_ioService.poll();
}

void Host::pingAll()
{
	Guard l(x_peers);
	for (auto& i: m_peers)
		if (auto j = i.second.lock())
			j->ping();
}

bytes Host::savePeers() const
{
	Guard l(x_peers);
	RLPStream ret;
	int n = 0;
	for (auto& i: m_peers)
		if (auto p = i.second.lock())
			if (p->m_socket.is_open() && p->endpoint().port())
			{
				ret.appendList(3) << p->endpoint().address().to_v4().to_bytes() << p->endpoint().port() << p->m_id;
				n++;
			}
	return RLPStream(n).appendRaw(ret.out(), n).out();
}

void Host::restorePeers(bytesConstRef _b)
{
	for (auto i: RLP(_b))
	{
		auto k = (h512)i[2];
		if (!m_incomingPeers.count(k))
		{
			m_incomingPeers.insert(make_pair(k, make_pair(bi::tcp::endpoint(bi::address_v4(i[0].toArray<byte, 4>()), i[1].toInt<short>()), 0)));
			m_freePeers.push_back(k);
		}
	}
}
