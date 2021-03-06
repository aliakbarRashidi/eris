package trie

import "github.com/eris-ltd/epm-go/Godeps/_workspace/src/github.com/eris-ltd/go-ethereum/ethutil"

type HashNode struct {
	key  []byte
	trie *Trie
}

func NewHash(key []byte, trie *Trie) *HashNode {
	return &HashNode{key, trie}
}

func (self *HashNode) RlpData() interface{} {
	return self.key
}

func (self *HashNode) Hash() interface{} {
	return self.key
}

// These methods will never be called but we have to satisfy Node interface
func (self *HashNode) Value() Node       { return nil }
func (self *HashNode) Dirty() bool       { return true }
func (self *HashNode) Copy(t *Trie) Node { return NewHash(ethutil.CopyBytes(self.key), t) }
