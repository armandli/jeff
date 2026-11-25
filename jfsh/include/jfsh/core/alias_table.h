#ifndef JFSH_CORE_ALIAS_TABLE_H
#define JFSH_CORE_ALIAS_TABLE_H
// jfsh/core/alias_table.h — the shell's aliases (`alias`, `unalias`).
//
// Mirrors zsh's aliastab and sufaliastab.  The parser expands them as it
// reads command words (Parser::expand_alias); the builtins edit them.

#include <string>

#include <jfsh/core/aliases.h>
#include <jfsh/core/types.h>

namespace jfsh {

struct AliasDef {
  str  text;
  bool global = false;   // -g: expands in any word position
};

struct AliasTable {
  HashTable<AliasDef> regular;   // regular and global aliases by name
  HashTable<str>      suffix;    // -s: file extension → command text
};

// The aliases every shell starts with (zsh's createaliastable): run-help
// and which-command, which the widgets of those names run.
AliasTable default_aliases();

}  // namespace jfsh

#endif  // JFSH_CORE_ALIAS_TABLE_H
