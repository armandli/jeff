#ifndef JFSH_CORE_OPTIONS_H
#define JFSH_CORE_OPTIONS_H
// jfsh/core/options.h — shell options (setopt / unsetopt).
//
// Mirrors zsh's options.c and the Options array indexed by OptIndex.
// Every zsh option is reproduced here so jfsh accepts the same setopt/unsetopt
// names and behaves identically.

#include <cstdint>
#include <cstdlib>

#include <array>
#include <functional>
#include <optional>
#include <string_view>

#include <jfsh/core/aliases.h>

namespace jfsh {

// One entry per zsh 5.9 option, in `set -o` order.
enum OptIndex : s::uint16_t {
  OPT_ALIASES = 0,
  OPT_ALIASFUNCDEF,
  OPT_ALLEXPORT,
  OPT_ALWAYSLASTPROMPT,
  OPT_ALWAYSTOEND,
  OPT_APPENDCREATE,
  OPT_APPENDHISTORY,
  OPT_AUTOCD,
  OPT_AUTOCONTINUE,
  OPT_AUTOLIST,
  OPT_AUTOMENU,
  OPT_AUTONAMEDIRS,
  OPT_AUTOPARAMKEYS,
  OPT_AUTOPARAMSLASH,
  OPT_AUTOPUSHD,
  OPT_AUTOREMOVESLASH,
  OPT_AUTORESUME,
  OPT_BADPATTERN,
  OPT_BANGHIST,
  OPT_BAREGLOBQUAL,
  OPT_BASHAUTOLIST,
  OPT_BASHREMATCH,
  OPT_BEEP,
  OPT_BGNICE,
  OPT_BRACECCL,
  OPT_BSDECHO,
  OPT_CASEGLOB,
  OPT_CASEMATCH,
  OPT_CASEPATHS,
  OPT_CBASES,
  OPT_CDABLEVARS,
  OPT_CDSILENT,
  OPT_CHASEDOTS,
  OPT_CHASELINKS,
  OPT_CHECKJOBS,
  OPT_CHECKRUNNINGJOBS,
  OPT_CLOBBER,
  OPT_CLOBBEREMPTY,
  OPT_COMBININGCHARS,
  OPT_COMPLETEALIASES,
  OPT_COMPLETEINWORD,
  OPT_CONTINUEONERROR,
  OPT_CORRECT,
  OPT_CORRECTALL,
  OPT_CPRECEDENCES,
  OPT_CSHJUNKIEHISTORY,
  OPT_CSHJUNKIELOOPS,
  OPT_CSHJUNKIEQUOTES,
  OPT_CSHNULLCMD,
  OPT_CSHNULLGLOB,
  OPT_DEBUGBEFORECMD,
  OPT_DVORAK,
  OPT_EMACS,
  OPT_EQUALS,
  OPT_ERREXIT,
  OPT_ERRRETURN,
  OPT_EVALLINENO,
  OPT_EXEC,
  OPT_EXTENDEDGLOB,
  OPT_EXTENDEDHISTORY,
  OPT_FLOWCONTROL,
  OPT_FORCEFLOAT,
  OPT_FUNCTIONARGZERO,
  OPT_GLOB,
  OPT_GLOBALEXPORT,
  OPT_GLOBALRCS,
  OPT_GLOBASSIGN,
  OPT_GLOBCOMPLETE,
  OPT_GLOBDOTS,
  OPT_GLOBSTARSHORT,
  OPT_GLOBSUBST,
  OPT_HASHCMDS,
  OPT_HASHDIRS,
  OPT_HASHEXECUTABLESONLY,
  OPT_HASHLISTALL,
  OPT_HISTALLOWCLOBBER,
  OPT_HISTBEEP,
  OPT_HISTEXPIREDUPSFIRST,
  OPT_HISTFCNTLLOCK,
  OPT_HISTFINDNODUPS,
  OPT_HISTIGNOREALLDUPS,
  OPT_HISTIGNOREDUPS,
  OPT_HISTIGNORESPACE,
  OPT_HISTLEXWORDS,
  OPT_HISTNOFUNCTIONS,
  OPT_HISTNOSTORE,
  OPT_HISTREDUCEBLANKS,
  OPT_HISTSAVEBYCOPY,
  OPT_HISTSAVENODUPS,
  OPT_HISTSUBSTPATTERN,
  OPT_HISTVERIFY,
  OPT_HUP,
  OPT_IGNOREBRACES,
  OPT_IGNORECLOSEBRACES,
  OPT_IGNOREEOF,
  OPT_INCAPPENDHISTORY,
  OPT_INCAPPENDHISTORYTIME,
  OPT_INTERACTIVE,
  OPT_INTERACTIVECOMMENTS,
  OPT_KSH_ARRAYS,
  OPT_KSH_AUTOLOAD,
  OPT_KSH_GLOB,
  OPT_KSH_OPTION_PRINT,
  OPT_KSH_TYPESET,
  OPT_KSH_ZERO_SUBSCRIPT,
  OPT_LISTAMBIGUOUS,
  OPT_LISTBEEP,
  OPT_LISTPACKED,
  OPT_LISTROWSFIRST,
  OPT_LISTTYPES,
  OPT_LOCALLOOPS,
  OPT_LOCALOPTIONS,
  OPT_LOCALPATTERNS,
  OPT_LOCALTRAPS,
  OPT_LOGIN,
  OPT_LONGLISTJOBS,
  OPT_MAGICEQUALSUBST,
  OPT_MAILWARNING,
  OPT_MARKDIRS,
  OPT_MENUCOMPLETE,
  OPT_MONITOR,
  OPT_MULTIBYTE,
  OPT_MULTIFUNCDEF,
  OPT_MULTIOS,
  OPT_NOMATCH,
  OPT_NOTIFY,
  OPT_NULLGLOB,
  OPT_NUMERICGLOBSORT,
  OPT_OCTALZEROES,
  OPT_OVERSTRIKE,
  OPT_PATHDIRS,
  OPT_PATHSCRIPT,
  OPT_PIPEFAIL,
  OPT_POSIXALIASES,
  OPT_POSIXARGZERO,
  OPT_POSIXBUILTINS,
  OPT_POSIXCD,
  OPT_POSIXIDENTIFIERS,
  OPT_POSIXJOBS,
  OPT_POSIXSTRINGS,
  OPT_POSIXTRAPS,
  OPT_PRINTEIGHTBIT,
  OPT_PRINTEXITVALUE,
  OPT_PRIVILEGED,
  OPT_PROMPTBANG,
  OPT_PROMPTCR,
  OPT_PROMPTPERCENT,
  OPT_PROMPTSP,
  OPT_PROMPTSUBST,
  OPT_PUSHDIGNOREDUPS,
  OPT_PUSHDMINUS,
  OPT_PUSHDSILENT,
  OPT_PUSHDTOHOME,
  OPT_RCEXPANDPARAM,
  OPT_RCQUOTES,
  OPT_RCS,
  OPT_RECEXACT,
  OPT_REMATCHPCRE,
  OPT_RESTRICTED,
  OPT_RMSTARSILENT,
  OPT_RMSTARWAIT,
  OPT_SHAREHISTORY,
  OPT_SHFILEEXPANSION,
  OPT_SHGLOB,
  OPT_SHINSTDIN,
  OPT_SHNULLCMD,
  OPT_SHOPTIONLETTERS,
  OPT_SHORTLOOPS,
  OPT_SHORTREPEAT,
  OPT_SHWORDSPLIT,
  OPT_SINGLECOMMAND,
  OPT_SINGLELINEZLE,
  OPT_SOURCETRACE,
  OPT_SUNKEYBOARDHACK,
  OPT_TRANSIENTRPROMPT,
  OPT_TRAPSASYNC,
  OPT_TYPESETSILENT,
  OPT_TYPESETTOUNSET,
  OPT_UNSET,
  OPT_VERBOSE,
  OPT_VI,
  OPT_WARNCREATEGLOBAL,
  OPT_WARNNESTEDVAR,
  OPT_XTRACE,
  OPT_ZLE,
  OPT_COUNT,
};

struct OptionInfo {
  const char* name;           // canonical lowercase name
  OptIndex    index;
  bool        default_val;    // default in native zsh mode
  bool        emulation_sh;   // default in sh emulation
  bool        emulation_ksh;  // default in ksh emulation
  bool        emulation_csh;  // default in csh emulation
  // Reset by a plain `emulate` (zsh's OPT_EMULATE); `emulate -R` resets
  // every option but the special ones.
  bool        emulate;
  // Set by how the shell was started (interactive, login, …): `emulate`
  // leaves it alone and `set -o`/`setopt` can't change most of them.
  bool        special;
};

// What a single-letter flag (`set -e`, `$-`) stands for: an option, or its
// negation (`set -u` is NO_UNSET).
struct OptionLetter {
  OptIndex index;
  bool     negated;
};

struct OptionState {
  OptionState();

  enum class Emulation : int { ZSH, SH, KSH, CSH };

  bool get(OptIndex option)        const noexcept { return mOpts[option]; }
  void set(OptIndex option)              noexcept { mOpts[option] = true; }
  void unset(OptIndex option)            noexcept { mOpts[option] = false; }
  void assign(OptIndex option, bool val) noexcept { mOpts[option] = val; }

  // Accepts "extendedglob", "EXTENDED_GLOB", etc.  A name that is not an
  // option but is one with `no` in front ("noextendedglob") finds that
  // option and sets `negated`.
  static optref<const OptionInfo>
  find(strv name, bool* negated = nullptr) noexcept;

  // The option a flag letter stands for: zsh's letters, or with
  // SH_OPTION_LETTERS the sh/ksh ones.  nullopt for a letter that is none.
  opt<OptionLetter> letter(char c) const noexcept;

  // $-: the letters of the options that are on, as zsh orders them.
  str letters() const;

  // `emulate mode`: the emulation options to the mode's defaults; with
  // `full` (`emulate -R`) every option but the special ones.
  void apply_emulation(Emulation, bool full = false);
  Emulation emulation() const noexcept { return mEmulation; }
  // The default of `info`'s option in the current emulation.
  bool default_of(const OptionInfo& info) const noexcept;

  bool extended_glob() const noexcept { return mOpts[OPT_EXTENDEDGLOB]; }
  // MULTIBYTE in a locale whose characters may take several bytes.
  bool multibyte() const noexcept { return mOpts[OPT_MULTIBYTE] and MB_CUR_MAX > 1; }
  bool monitor()       const noexcept { return mOpts[OPT_MONITOR]; }
  bool interactive()   const noexcept { return mOpts[OPT_INTERACTIVE]; }
  bool login()         const noexcept { return mOpts[OPT_LOGIN]; }
  bool pipefail()      const noexcept { return mOpts[OPT_PIPEFAIL]; }
  bool nullglob()      const noexcept { return mOpts[OPT_NULLGLOB]; }
  bool nomatch()       const noexcept { return mOpts[OPT_NOMATCH]; }
  bool errexit()       const noexcept { return mOpts[OPT_ERREXIT]; }
  bool xtrace()        const noexcept { return mOpts[OPT_XTRACE]; }

private:
  s::array<bool, OPT_COUNT> mOpts{};
  Emulation                 mEmulation = Emulation::ZSH;
};

extern const OptionInfo OPTION_INFO[OPT_COUNT];

}  // namespace jfsh

#endif  // JFSH_CORE_OPTIONS_H
