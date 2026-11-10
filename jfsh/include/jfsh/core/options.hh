#pragma once
// jfsh/core/options.hh — shell options (setopt / unsetopt).
//
// Mirrors zsh's options.c and the Options array indexed by OptIndex.
// Every zsh option is reproduced here so jfsh accepts the same setopt/unsetopt
// names and behaves identically.

#include <cstdint>
#include <string_view>
#include <array>

namespace jfsh {

// ---------------------------------------------------------------------------
// Option indices — one per zsh option, in alphabetical order.
// Keep in sync with kOptionInfo[] in options.cpp.
// ---------------------------------------------------------------------------
enum OptIndex : std::uint16_t {
    // A
    OPT_ALIASES = 0,
    OPT_ALLEXPORT,
    OPT_ALWAYSLASTPROMPT,
    OPT_ALWAYSTOEND,
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
    // B
    OPT_BAREGLOBQUAL,
    OPT_BEEP,
    OPT_BGNICE,
    OPT_BRACECCL,
    OPT_BSDECHO,
    // C
    OPT_CASEGLOB,
    OPT_CASEMATCH,
    OPT_CBASES,
    OPT_CDABLEVARS,
    OPT_CHASEDOTS,
    OPT_CHASELINKS,
    OPT_CHECKJOBS,
    OPT_CHECKRUNNINGJOBS,
    OPT_CLOBBER,
    OPT_COMBININGCHARS,
    OPT_COMPLETEALIASES,
    OPT_COMPLETEINWORD,
    OPT_CORRECT,
    OPT_CORRECTALL,
    OPT_CPRECEDENCES,
    OPT_CSHJUNKIEHISTORY,
    OPT_CSHJUNKIELOOPS,
    OPT_CSHJUNKIEQUOTES,
    OPT_CSHJOINCTRLS,
    OPT_CSHNULLCMD,
    OPT_CSHNULLGLOB,
    // D
    OPT_DEBUGBEFORECMD,
    OPT_DOTGLOB,
    OPT_DVORAK,
    // E
    OPT_EMACS,
    OPT_EQUALS,
    OPT_ERREXIT,
    OPT_ERRRETURN,
    OPT_EVALLINENO,
    OPT_EXEC,
    OPT_EXTENDEDGLOB,
    OPT_EXTENDEDHISTORY,
    // F
    OPT_FLOWCONTROL,
    OPT_FORCEFLOAT,
    OPT_FUNCTIONARGZERO,
    // G
    OPT_GLOBALEXPORT,
    OPT_GLOBALRCS,
    OPT_GLOBASSIGN,
    OPT_GLOBCOMPLETE,
    OPT_GLOBDOTS,
    OPT_GLOBSUBST,
    // H
    OPT_HASHCMDS,
    OPT_HASHDIRS,
    OPT_HASHEXECUTABLESONLY,
    OPT_HISTALLOWCLOBBER,
    OPT_HISTBEEP,
    OPT_HISTEXPIREDUPSFIRST,
    OPT_HISTFINDNODUPS,
    OPT_HISTIGNOREALLDUPS,
    OPT_HISTIGNOREDUPS,
    OPT_HISTIGNORESPACE,
    OPT_HISTLEXWORDS,
    OPT_HISTNODUPS,
    OPT_HISTNOSTORE,
    OPT_HISTREDUCEBLANKS,
    OPT_HISTSAVEBYCOPY,
    OPT_HISTSAVENODUPS,
    OPT_HISTSUBSTPATTERN,
    OPT_HISTVERIFY,
    OPT_HUP,
    // I
    OPT_IGNOREBRACES,
    OPT_IGNORECLOSEBRACES,
    OPT_IGNOREEOF,
    // J K
    OPT_INTERACTIVECOMMENTS,
    // L
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
    OPT_LOGCHECK,
    OPT_LOGIN,
    OPT_LONGLISTJOBS,
    // M
    OPT_MAGICEQUALSUBST,
    OPT_MAILWARN,
    OPT_MARKDIRS,
    OPT_MENUCOMPLETE,
    OPT_MONITOR,
    OPT_MULTIBYTE,
    OPT_MULTIFUNCDEF,
    OPT_MULTIOS,
    // N
    OPT_NOMATCH,
    OPT_NOTIFY,
    OPT_NULLGLOB,
    OPT_NUMERICGLOBSORT,
    // O
    OPT_OCTALZEROES,
    OPT_OVERSTRIKE,
    // P
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
    // R
    OPT_RCEXPANDPARAM,
    OPT_RCQUOTES,
    OPT_RCS,
    OPT_RECEXACT,
    OPT_REMATCHPCRE,
    OPT_RESTRICTED,
    OPT_RMSTARSILENT,
    OPT_RMSTARWAIT,
    // S
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
    OPT_STDIN,
    OPT_SUNKEYBOARDHACK,
    // T
    OPT_TRANSIENTRPROMPT,
    OPT_TRAPSASYNC,
    OPT_TYPESETSILENT,
    // U
    OPT_UNSET,
    OPT_VERBOSE,
    // V
    OPT_VI,
    // W
    OPT_WARNCREATEGLOBAL,
    OPT_WARNNESTEDVAR,
    // X
    OPT_XTRACE,
    // Z
    OPT_ZLEBINDINGS,
    // Count must be last
    OPT_COUNT
};

// ---------------------------------------------------------------------------
// OptionState — the live option settings for the running shell
// ---------------------------------------------------------------------------
struct OptionInfo {
    const char*  name;          // canonical lowercase name
    OptIndex     index;
    bool         default_val;   // default for interactive zsh session
    bool         emulation_sh;  // default in sh emulation
    bool         emulation_ksh; // default in ksh emulation
    bool         emulation_csh; // default in csh emulation
};

class OptionState {
public:
    OptionState();

    bool  get(OptIndex opt)  const noexcept { return opts_[opt]; }
    void  set(OptIndex opt)              noexcept { opts_[opt] = true; }
    void  unset(OptIndex opt)            noexcept { opts_[opt] = false; }
    void  assign(OptIndex opt, bool val) noexcept { opts_[opt] = val;  }

    // Lookup by name ("extendedglob", "EXTENDED_GLOB", etc.)
    // Returns nullptr if not found.
    static const OptionInfo* find(std::string_view name) noexcept;

    // Apply emulation mode defaults.
    enum class Emulation { ZSH, SH, KSH, CSH };
    void apply_emulation(Emulation);

    // Convenience
    bool extended_glob()   const noexcept { return opts_[OPT_EXTENDEDGLOB]; }
    bool monitor()         const noexcept { return opts_[OPT_MONITOR]; }
    bool interactive()     const noexcept { return opts_[OPT_STDIN]; }
    bool login()           const noexcept { return opts_[OPT_LOGIN]; }
    bool pipefail()        const noexcept { return opts_[OPT_PIPEFAIL]; }
    bool nullglob()        const noexcept { return opts_[OPT_NULLGLOB]; }
    bool nomatch()         const noexcept { return opts_[OPT_NOMATCH]; }
    bool errexit()         const noexcept { return opts_[OPT_ERREXIT]; }
    bool xtrace()          const noexcept { return opts_[OPT_XTRACE]; }

private:
    std::array<bool, OPT_COUNT> opts_{};
};

// ---------------------------------------------------------------------------
// Global option table (populated by options.cpp)
// ---------------------------------------------------------------------------
extern const OptionInfo kOptionInfo[OPT_COUNT];

} // namespace jfsh
