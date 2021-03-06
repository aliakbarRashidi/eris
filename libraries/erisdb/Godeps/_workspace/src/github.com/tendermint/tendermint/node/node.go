package node

import (
	"fmt"
	"math/rand"
	"net"
	"net/http"
	"os"
	"strconv"

	bc "github.com/eris-ltd/epm-go/Godeps/_workspace/src/github.com/tendermint/tendermint/blockchain"
	. "github.com/eris-ltd/epm-go/Godeps/_workspace/src/github.com/tendermint/tendermint/common"
	"github.com/eris-ltd/epm-go/Godeps/_workspace/src/github.com/tendermint/tendermint/config"
	"github.com/eris-ltd/epm-go/Godeps/_workspace/src/github.com/tendermint/tendermint/consensus"
	dbm "github.com/eris-ltd/epm-go/Godeps/_workspace/src/github.com/tendermint/tendermint/db"
	"github.com/eris-ltd/epm-go/Godeps/_workspace/src/github.com/tendermint/tendermint/events"
	mempl "github.com/eris-ltd/epm-go/Godeps/_workspace/src/github.com/tendermint/tendermint/mempool"
	"github.com/eris-ltd/epm-go/Godeps/_workspace/src/github.com/tendermint/tendermint/p2p"
	"github.com/eris-ltd/epm-go/Godeps/_workspace/src/github.com/tendermint/tendermint/rpc"
	"github.com/eris-ltd/epm-go/Godeps/_workspace/src/github.com/tendermint/tendermint/rpc/core"
	sm "github.com/eris-ltd/epm-go/Godeps/_workspace/src/github.com/tendermint/tendermint/state"
	"github.com/eris-ltd/epm-go/Godeps/_workspace/src/github.com/tendermint/tendermint/types"
)

import _ "net/http/pprof"

func init() {
	go func() {
		fmt.Println(http.ListenAndServe("0.0.0.0:6060", nil))
	}()
}

type Node struct {
	sw               *p2p.Switch
	evsw             *events.EventSwitch
	book             *p2p.AddrBook
	blockStore       *bc.BlockStore
	pexReactor       *p2p.PEXReactor
	bcReactor        *bc.BlockchainReactor
	mempoolReactor   *mempl.MempoolReactor
	consensusState   *consensus.ConsensusState
	consensusReactor *consensus.ConsensusReactor
	privValidator    *sm.PrivValidator
}

func NewNode() *Node {
	// Get BlockStore
	blockStoreDB := dbm.GetDB("blockstore")
	blockStore := bc.NewBlockStore(blockStoreDB)

	// Get State
	stateDB := dbm.GetDB("state")
	state := sm.LoadState(stateDB)
	if state == nil {
		state = sm.MakeGenesisStateFromFile(stateDB, config.App().GetString("GenesisFile"))
		state.Save()
	}

	// Get PrivValidator
	var privValidator *sm.PrivValidator
	privValidatorFile := config.App().GetString("PrivValidatorFile")
	if _, err := os.Stat(privValidatorFile); err == nil {
		privValidator = sm.LoadPrivValidator(privValidatorFile)
		log.Info("Loaded PrivValidator",
			"file", privValidatorFile, "privValidator", privValidator)
	} else {
		privValidator = sm.GenPrivValidator()
		privValidator.SetFile(privValidatorFile)
		privValidator.Save()
		log.Info("Generated PrivValidator", "file", privValidatorFile)
	}

	eventSwitch := new(events.EventSwitch)
	eventSwitch.Start()

	// Get PEXReactor
	book := p2p.NewAddrBook(config.App().GetString("AddrBookFile"))
	pexReactor := p2p.NewPEXReactor(book)

	// Get BlockchainReactor
	bcReactor := bc.NewBlockchainReactor(state, blockStore, config.App().GetBool("FastSync"))

	// Get MempoolReactor
	mempool := mempl.NewMempool(state.Copy())
	mempoolReactor := mempl.NewMempoolReactor(mempool)

	// Get ConsensusReactor
	consensusState := consensus.NewConsensusState(state, blockStore, mempoolReactor)
	consensusReactor := consensus.NewConsensusReactor(consensusState, blockStore)
	if privValidator != nil {
		consensusReactor.SetPrivValidator(privValidator)
	}

	// so the consensus reactor won't do anything until we're synced
	if config.App().GetBool("FastSync") {
		consensusReactor.SetSyncing(true)
	}

	sw := p2p.NewSwitch()
	sw.AddReactor("PEX", pexReactor)
	sw.AddReactor("MEMPOOL", mempoolReactor)
	sw.AddReactor("BLOCKCHAIN", bcReactor)
	sw.AddReactor("CONSENSUS", consensusReactor)

	// add the event switch to all services
	// they should all satisfy events.Eventable
	SetFireable(eventSwitch, pexReactor, bcReactor, mempoolReactor, consensusReactor)

	return &Node{
		sw:               sw,
		evsw:             eventSwitch,
		book:             book,
		blockStore:       blockStore,
		pexReactor:       pexReactor,
		bcReactor:        bcReactor,
		mempoolReactor:   mempoolReactor,
		consensusState:   consensusState,
		consensusReactor: consensusReactor,
		privValidator:    privValidator,
	}
}

// Call Start() after adding the listeners.
func (n *Node) Start() {
	log.Info("Starting Node")
	n.book.Start()
	nodeInfo := makeNodeInfo(n.sw)
	n.sw.SetNodeInfo(nodeInfo)
	n.sw.Start()
}

func (n *Node) Stop() {
	log.Info("Stopping Node")
	// TODO: gracefully disconnect from peers.
	n.sw.Stop()
	n.book.Stop()
}

// Add the event switch to reactors, mempool, etc.
func SetFireable(evsw *events.EventSwitch, eventables ...events.Eventable) {
	for _, e := range eventables {
		e.SetFireable(evsw)
	}
}

// Add a Listener to accept inbound peer connections.
// Add listeners before starting the Node.
// The first listener is the primary listener (in NodeInfo)
func (n *Node) AddListener(l p2p.Listener) {
	log.Info(Fmt("Added %v", l))
	n.sw.AddListener(l)
	n.book.AddOurAddress(l.ExternalAddress())
}

func (n *Node) DialSeed() {
	// if the single seed node is available, use only it
	prioritySeed := config.App().GetString("SeedNode")
	if prioritySeed != "" {
		addr := p2p.NewNetAddressString(prioritySeed)
		n.dialSeed(addr)
		return
	}

	// permute the list, dial half of them
	seeds := config.App().GetStringSlice("SeedNodes")
	perm := rand.Perm(len(seeds))
	// TODO: we shouldn't necessarily connect to all of them every time ...
	for i := 0; i < len(perm); i++ {
		j := perm[i]
		addr := p2p.NewNetAddressString(seeds[j])
		n.dialSeed(addr)
	}
}

func (n *Node) dialSeed(addr *p2p.NetAddress) {
	peer, err := n.sw.DialPeerWithAddress(addr)
	if err != nil {
		log.Error("Error dialing seed", "error", err)
		//n.book.MarkAttempt(addr)
		return
	} else {
		log.Info("Connected to seed", "peer", peer)
		n.book.AddAddress(addr, addr)
	}
}

func (n *Node) StartRPC() {
	core.SetBlockStore(n.blockStore)
	core.SetConsensusState(n.consensusState)
	core.SetConsensusReactor(n.consensusReactor)
	core.SetMempoolReactor(n.mempoolReactor)
	core.SetSwitch(n.sw)

	listenAddr := config.App().GetString("RPC.HTTP.ListenAddr")
	mux := http.NewServeMux()
	rpc.RegisterEventsHandler(mux, n.evsw)
	rpc.RegisterRPCFuncs(mux, core.Routes)
	rpc.StartHTTPServer(listenAddr, mux)
}

func (n *Node) Switch() *p2p.Switch {
	return n.sw
}

func (n *Node) ConsensusState() *consensus.ConsensusState {
	return n.consensusState
}

func (n *Node) MempoolReactor() *mempl.MempoolReactor {
	return n.mempoolReactor
}

func (n *Node) EventSwitch() *events.EventSwitch {
	return n.evsw
}

func makeNodeInfo(sw *p2p.Switch) *types.NodeInfo {
	nodeInfo := &types.NodeInfo{
		Moniker: config.App().GetString("Moniker"),
		Network: config.App().GetString("Network"),
		Version: "0.2.0", // Everything is in Big Endian.
	}
	if !sw.IsListening() {
		return nodeInfo
	}
	p2pListener := sw.Listeners()[0]
	p2pHost := p2pListener.ExternalAddress().IP.String()
	p2pPort := p2pListener.ExternalAddress().Port
	rpcListenAddr := config.App().GetString("RPC.HTTP.ListenAddr")
	_, rpcPortStr, _ := net.SplitHostPort(rpcListenAddr)
	rpcPort, err := strconv.Atoi(rpcPortStr)
	if err != nil {
		panic(Fmt("Expected numeric RPC.HTTP.ListenAddr port but got %v", rpcPortStr))
	}

	// We assume that the rpcListener has the same ExternalAddress.
	// This is probably true because both P2P and RPC listeners use UPnP.
	nodeInfo.Host = p2pHost
	nodeInfo.P2PPort = p2pPort
	nodeInfo.RPCPort = uint16(rpcPort)
	return nodeInfo
}

//------------------------------------------------------------------------------

func RunNode() {
	// Create & start node
	n := NewNode()
	l := p2p.NewDefaultListener("tcp", config.App().GetString("ListenAddr"), false)
	n.AddListener(l)
	n.Start()

	// If seedNode is provided by config, dial out.
	if config.App().GetString("SeedNode") != "" || len(config.App().GetStringSlice("SeedNodes")) != 0 {
		n.DialSeed()
	}

	// Run the RPC server.
	if config.App().GetString("RPC.HTTP.ListenAddr") != "" {
		n.StartRPC()
	}

	// Sleep forever and then...
	TrapSignal(func() {
		n.Stop()
	})
}
