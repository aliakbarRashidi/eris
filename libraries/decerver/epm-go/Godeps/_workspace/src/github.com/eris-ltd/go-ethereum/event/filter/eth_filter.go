package filter

// TODO make use of the generic filtering system

import (
	"sync"

	"github.com/eris-ltd/epm-go/Godeps/_workspace/src/github.com/eris-ltd/go-ethereum/core"
	"github.com/eris-ltd/epm-go/Godeps/_workspace/src/github.com/eris-ltd/go-ethereum/event"
	"github.com/eris-ltd/epm-go/Godeps/_workspace/src/github.com/eris-ltd/go-ethereum/state"
)

type FilterManager struct {
	eventMux *event.TypeMux

	filterMu sync.RWMutex
	filterId int
	filters  map[int]*core.Filter

	quit chan struct{}
}

func NewFilterManager(mux *event.TypeMux) *FilterManager {
	return &FilterManager{
		eventMux: mux,
		filters:  make(map[int]*core.Filter),
	}
}

func (self *FilterManager) Start() {
	go self.filterLoop()
}

func (self *FilterManager) Stop() {
	close(self.quit)
}

func (self *FilterManager) InstallFilter(filter *core.Filter) (id int) {
	self.filterMu.Lock()
	id = self.filterId
	self.filters[id] = filter
	self.filterId++
	self.filterMu.Unlock()
	return id
}

func (self *FilterManager) UninstallFilter(id int) {
	self.filterMu.Lock()
	delete(self.filters, id)
	self.filterMu.Unlock()
}

// GetFilter retrieves a filter installed using InstallFilter.
// The filter may not be modified.
func (self *FilterManager) GetFilter(id int) *core.Filter {
	self.filterMu.RLock()
	defer self.filterMu.RUnlock()
	return self.filters[id]
}

func (self *FilterManager) filterLoop() {
	// Subscribe to events
	events := self.eventMux.Subscribe(core.PendingBlockEvent{}, core.NewBlockEvent{}, state.Logs(nil))

out:
	for {
		select {
		case <-self.quit:
			break out
		case event := <-events.Chan():
			switch event := event.(type) {
			case core.NewBlockEvent:
				self.filterMu.RLock()
				for _, filter := range self.filters {
					if filter.BlockCallback != nil {
						filter.BlockCallback(event.Block)
					}
				}
				self.filterMu.RUnlock()

			case core.PendingBlockEvent:
				self.filterMu.RLock()
				for _, filter := range self.filters {
					if filter.PendingCallback != nil {
						filter.PendingCallback(event.Block)
					}
				}
				self.filterMu.RUnlock()

			case state.Logs:
				self.filterMu.RLock()
				for _, filter := range self.filters {
					if filter.LogsCallback != nil {
						msgs := filter.FilterLogs(event)
						if len(msgs) > 0 {
							filter.LogsCallback(msgs)
						}
					}
				}
				self.filterMu.RUnlock()
			}
		}
	}
}
