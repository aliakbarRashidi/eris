package source

import (
	"github.com/eris-ltd/epm-go/Godeps/_workspace/src/github.com/spf13/pflag"
)

// A configuration source for pflags.
type PFlagSource struct {
	data map[string]*pflag.Flag
}

func NewPFlagSource() *PFlagSource {
	return &PFlagSource{
		data: make(map[string]*pflag.Flag),
	}
}

func (self *PFlagSource) Get(key string) (interface{}, bool) {
	val, exists := self.data[key]
	if exists == false {
		return nil, false
	}

	if val.Changed {
		return val.Value.String(), exists
	} else {
		return val.Value.String(), false
	}
}

func (self *PFlagSource) Set(key string, val interface{}) {
	self.data[key] = val.(*pflag.Flag)
}
