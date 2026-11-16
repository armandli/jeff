#include <jfsh/core/options.h>

#include <cctype>
#include <cstring>

#include <algorithm>
#include <string>

#include <jfsh/core/aliases.h>

namespace jfsh {

// Generated from zsh 5.9 (`zsh -f -c`): each mode's defaults are what
// `emulate E; set -o` marks as default (a `no` prefix: on by default).
// `emulate` resets only the options marked so (zsh's OPT_EMULATE);
// `emulate -R` resets all but the special ones, which the shell's
// invocation sets.
//  name                    index                     zsh    sh     ksh    csh    emulate special
const OptionInfo OPTION_INFO[OPT_COUNT] = {
  {"aliases",              OPT_ALIASES,              true , true , true , true , true , false},
  {"aliasfuncdef",         OPT_ALIASFUNCDEF,         false, true , true , false, true , false},
  {"allexport",            OPT_ALLEXPORT,            false, false, false, false, true , false},
  {"alwayslastprompt",     OPT_ALWAYSLASTPROMPT,     true , true , true , true , false, false},
  {"alwaystoend",          OPT_ALWAYSTOEND,          false, false, false, false, false, false},
  {"appendcreate",         OPT_APPENDCREATE,         false, true , true , false, true , false},
  {"appendhistory",        OPT_APPENDHISTORY,        true , true , true , true , false, false},
  {"autocd",               OPT_AUTOCD,               false, false, false, false, true , false},
  {"autocontinue",         OPT_AUTOCONTINUE,         false, false, false, false, false, false},
  {"autolist",             OPT_AUTOLIST,             true , true , true , true , false, false},
  {"automenu",             OPT_AUTOMENU,             true , true , true , true , false, false},
  {"autonamedirs",         OPT_AUTONAMEDIRS,         false, false, false, false, false, false},
  {"autoparamkeys",        OPT_AUTOPARAMKEYS,        true , true , true , true , false, false},
  {"autoparamslash",       OPT_AUTOPARAMSLASH,       true , true , true , true , false, false},
  {"autopushd",            OPT_AUTOPUSHD,            false, false, false, false, false, false},
  {"autoremoveslash",      OPT_AUTOREMOVESLASH,      true , true , true , true , false, false},
  {"autoresume",           OPT_AUTORESUME,           false, false, false, false, false, false},
  {"badpattern",           OPT_BADPATTERN,           true , false, false, true , true , false},
  {"banghist",             OPT_BANGHIST,             true , false, false, true , false, false},
  {"bareglobqual",         OPT_BAREGLOBQUAL,         true , false, false, false, true , false},
  {"bashautolist",         OPT_BASHAUTOLIST,         false, false, false, false, false, false},
  {"bashrematch",          OPT_BASHREMATCH,          false, false, false, false, false, false},
  {"beep",                 OPT_BEEP,                 true , true , true , true , false, false},
  {"bgnice",               OPT_BGNICE,               true , false, false, true , true , false},
  {"braceccl",             OPT_BRACECCL,             false, false, false, false, true , false},
  {"bsdecho",              OPT_BSDECHO,              false, true , false, false, true , false},
  {"caseglob",             OPT_CASEGLOB,             true , true , true , true , false, false},
  {"casematch",            OPT_CASEMATCH,            true , true , true , true , false, false},
  {"casepaths",            OPT_CASEPATHS,            false, false, false, false, false, false},
  {"cbases",               OPT_CBASES,               false, false, false, false, false, false},
  {"cdablevars",           OPT_CDABLEVARS,           false, false, false, false, true , false},
  {"cdsilent",             OPT_CDSILENT,             false, false, false, false, false, false},
  {"chasedots",            OPT_CHASEDOTS,            false, false, false, false, true , false},
  {"chaselinks",           OPT_CHASELINKS,           false, false, false, false, true , false},
  {"checkjobs",            OPT_CHECKJOBS,            true , false, false, false, true , false},
  {"checkrunningjobs",     OPT_CHECKRUNNINGJOBS,     true , false, false, false, true , false},
  {"clobber",              OPT_CLOBBER,              true , true , true , true , true , false},
  {"clobberempty",         OPT_CLOBBEREMPTY,         false, false, false, false, false, false},
  {"combiningchars",       OPT_COMBININGCHARS,       false, false, false, false, false, false},
  {"completealiases",      OPT_COMPLETEALIASES,      false, false, false, false, false, false},
  {"completeinword",       OPT_COMPLETEINWORD,       false, false, false, false, false, false},
  {"continueonerror",      OPT_CONTINUEONERROR,      false, false, false, false, false, false},
  {"correct",              OPT_CORRECT,              false, false, false, false, false, false},
  {"correctall",           OPT_CORRECTALL,           false, false, false, false, false, false},
  {"cprecedences",         OPT_CPRECEDENCES,         false, true , true , true , true , false},
  {"cshjunkiehistory",     OPT_CSHJUNKIEHISTORY,     false, false, false, true , true , false},
  {"cshjunkieloops",       OPT_CSHJUNKIELOOPS,       false, false, false, true , true , false},
  {"cshjunkiequotes",      OPT_CSHJUNKIEQUOTES,      false, false, false, true , true , false},
  {"cshnullcmd",           OPT_CSHNULLCMD,           false, false, false, true , true , false},
  {"cshnullglob",          OPT_CSHNULLGLOB,          false, false, false, true , true , false},
  {"debugbeforecmd",       OPT_DEBUGBEFORECMD,       true , true , true , true , false, false},
  {"dvorak",               OPT_DVORAK,               false, false, false, false, false, false},
  {"emacs",                OPT_EMACS,                false, false, false, false, true , false},
  {"equals",               OPT_EQUALS,               true , false, false, false, true , false},
  {"errexit",              OPT_ERREXIT,              false, false, false, false, true , false},
  {"errreturn",            OPT_ERRRETURN,            false, false, false, false, true , false},
  {"evallineno",           OPT_EVALLINENO,           true , false, false, false, true , false},
  {"exec",                 OPT_EXEC,                 true , true , true , true , true , false},
  {"extendedglob",         OPT_EXTENDEDGLOB,         false, false, false, false, true , false},
  {"extendedhistory",      OPT_EXTENDEDHISTORY,      false, false, false, true , false, false},
  {"flowcontrol",          OPT_FLOWCONTROL,          true , true , true , true , false, false},
  {"forcefloat",           OPT_FORCEFLOAT,           false, false, false, false, false, false},
  {"functionargzero",      OPT_FUNCTIONARGZERO,      true , false, false, true , true , false},
  {"glob",                 OPT_GLOB,                 true , true , true , true , true , false},
  {"globalexport",         OPT_GLOBALEXPORT,         true , false, false, false, true , false},
  {"globalrcs",            OPT_GLOBALRCS,            true , true , true , true , false, false},
  {"globassign",           OPT_GLOBASSIGN,           false, false, false, true , true , false},
  {"globcomplete",         OPT_GLOBCOMPLETE,         false, false, false, false, false, false},
  {"globdots",             OPT_GLOBDOTS,             false, false, false, false, true , false},
  {"globstarshort",        OPT_GLOBSTARSHORT,        false, false, false, false, true , false},
  {"globsubst",            OPT_GLOBSUBST,            false, true , true , true , true , false},
  {"hashcmds",             OPT_HASHCMDS,             true , true , true , true , false, false},
  {"hashdirs",             OPT_HASHDIRS,             true , true , true , true , false, true },
  {"hashexecutablesonly",  OPT_HASHEXECUTABLESONLY,  false, false, false, false, false, false},
  {"hashlistall",          OPT_HASHLISTALL,          true , true , true , true , false, false},
  {"histallowclobber",     OPT_HISTALLOWCLOBBER,     false, false, false, false, false, false},
  {"histbeep",             OPT_HISTBEEP,             true , true , true , true , false, false},
  {"histexpiredupsfirst",  OPT_HISTEXPIREDUPSFIRST,  false, false, false, false, false, false},
  {"histfcntllock",        OPT_HISTFCNTLLOCK,        false, false, false, false, false, false},
  {"histfindnodups",       OPT_HISTFINDNODUPS,       false, false, false, false, false, false},
  {"histignorealldups",    OPT_HISTIGNOREALLDUPS,    false, false, false, false, false, false},
  {"histignoredups",       OPT_HISTIGNOREDUPS,       false, false, false, false, false, false},
  {"histignorespace",      OPT_HISTIGNORESPACE,      false, false, false, false, false, false},
  {"histlexwords",         OPT_HISTLEXWORDS,         false, false, false, false, false, false},
  {"histnofunctions",      OPT_HISTNOFUNCTIONS,      false, false, false, false, false, false},
  {"histnostore",          OPT_HISTNOSTORE,          false, false, false, false, false, false},
  {"histreduceblanks",     OPT_HISTREDUCEBLANKS,     false, false, false, false, false, false},
  {"histsavebycopy",       OPT_HISTSAVEBYCOPY,       true , true , true , true , false, false},
  {"histsavenodups",       OPT_HISTSAVENODUPS,       false, false, false, false, false, false},
  {"histsubstpattern",     OPT_HISTSUBSTPATTERN,     false, false, false, false, true , false},
  {"histverify",           OPT_HISTVERIFY,           false, false, false, false, false, false},
  {"hup",                  OPT_HUP,                  true , false, false, false, true , false},
  {"ignorebraces",         OPT_IGNOREBRACES,         false, true , false, false, true , false},
  {"ignoreclosebraces",    OPT_IGNORECLOSEBRACES,    false, false, false, false, true , false},
  {"ignoreeof",            OPT_IGNOREEOF,            false, false, false, false, false, false},
  {"incappendhistory",     OPT_INCAPPENDHISTORY,     false, false, false, false, false, false},
  {"incappendhistorytime", OPT_INCAPPENDHISTORYTIME, false, false, false, false, false, false},
  {"interactive",          OPT_INTERACTIVE,          false, false, false, false, false, true },
  {"interactivecomments",  OPT_INTERACTIVECOMMENTS,  false, true , true , false, false, false},
  {"ksharrays",            OPT_KSH_ARRAYS,           false, true , true , false, true , false},
  {"kshautoload",          OPT_KSH_AUTOLOAD,         false, true , true , false, true , false},
  {"kshglob",              OPT_KSH_GLOB,             false, false, true , false, true , false},
  {"kshoptionprint",       OPT_KSH_OPTION_PRINT,     false, false, true , false, true , false},
  {"kshtypeset",           OPT_KSH_TYPESET,          false, false, false, false, false, false},
  {"kshzerosubscript",     OPT_KSH_ZERO_SUBSCRIPT,   false, false, false, false, false, false},
  {"listambiguous",        OPT_LISTAMBIGUOUS,        true , true , true , true , false, false},
  {"listbeep",             OPT_LISTBEEP,             true , true , true , true , false, false},
  {"listpacked",           OPT_LISTPACKED,           false, false, false, false, false, false},
  {"listrowsfirst",        OPT_LISTROWSFIRST,        false, false, false, false, false, false},
  {"listtypes",            OPT_LISTTYPES,            true , true , true , true , false, false},
  {"localloops",           OPT_LOCALLOOPS,           false, false, false, false, true , false},
  {"localoptions",         OPT_LOCALOPTIONS,         false, false, true , false, true , false},
  {"localpatterns",        OPT_LOCALPATTERNS,        false, false, false, false, true , false},
  {"localtraps",           OPT_LOCALTRAPS,           false, false, true , false, true , false},
  {"login",                OPT_LOGIN,                false, false, false, false, false, true },
  {"longlistjobs",         OPT_LONGLISTJOBS,         false, false, false, false, false, false},
  {"magicequalsubst",      OPT_MAGICEQUALSUBST,      false, false, false, false, true , false},
  {"mailwarning",          OPT_MAILWARNING,          false, false, false, false, false, false},
  {"markdirs",             OPT_MARKDIRS,             false, false, false, false, false, false},
  {"menucomplete",         OPT_MENUCOMPLETE,         false, false, false, false, false, false},
  {"monitor",              OPT_MONITOR,              false, false, false, false, false, true },
  {"multibyte",            OPT_MULTIBYTE,            true , true , true , true , false, false},
  {"multifuncdef",         OPT_MULTIFUNCDEF,         true , false, false, false, true , false},
  {"multios",              OPT_MULTIOS,              true , false, false, false, true , false},
  {"nomatch",              OPT_NOMATCH,              true , false, false, true , true , false},
  {"notify",               OPT_NOTIFY,               true , false, false, false, false, false},
  {"nullglob",             OPT_NULLGLOB,             false, false, false, false, true , false},
  {"numericglobsort",      OPT_NUMERICGLOBSORT,      false, false, false, false, true , false},
  {"octalzeroes",          OPT_OCTALZEROES,          false, true , false, false, true , false},
  {"overstrike",           OPT_OVERSTRIKE,           false, false, false, false, false, false},
  {"pathdirs",             OPT_PATHDIRS,             false, false, false, false, true , false},
  {"pathscript",           OPT_PATHSCRIPT,           false, true , true , false, true , false},
  {"pipefail",             OPT_PIPEFAIL,             false, false, false, false, true , false},
  {"posixaliases",         OPT_POSIXALIASES,         false, true , true , false, true , false},
  {"posixargzero",         OPT_POSIXARGZERO,         false, false, false, false, true , false},
  {"posixbuiltins",        OPT_POSIXBUILTINS,        false, true , true , false, true , false},
  {"posixcd",              OPT_POSIXCD,              false, true , true , false, true , false},
  {"posixidentifiers",     OPT_POSIXIDENTIFIERS,     false, true , true , false, true , false},
  {"posixjobs",            OPT_POSIXJOBS,            false, true , true , false, true , false},
  {"posixstrings",         OPT_POSIXSTRINGS,         false, true , true , false, true , false},
  {"posixtraps",           OPT_POSIXTRAPS,           false, true , true , false, true , false},
  {"printeightbit",        OPT_PRINTEIGHTBIT,        false, false, false, false, false, false},
  {"printexitvalue",       OPT_PRINTEXITVALUE,       false, false, false, false, false, false},
  {"privileged",           OPT_PRIVILEGED,           false, false, false, false, false, true },
  {"promptbang",           OPT_PROMPTBANG,           false, false, true , false, false, false},
  {"promptcr",             OPT_PROMPTCR,             true , true , true , true , false, false},
  {"promptpercent",        OPT_PROMPTPERCENT,        true , false, false, true , false, false},
  {"promptsp",             OPT_PROMPTSP,             true , true , true , true , false, false},
  {"promptsubst",          OPT_PROMPTSUBST,          false, true , true , false, false, false},
  {"pushdignoredups",      OPT_PUSHDIGNOREDUPS,      false, false, false, false, true , false},
  {"pushdminus",           OPT_PUSHDMINUS,           false, false, false, false, true , false},
  {"pushdsilent",          OPT_PUSHDSILENT,          false, false, false, false, false, false},
  {"pushdtohome",          OPT_PUSHDTOHOME,          false, false, false, false, true , false},
  {"rcexpandparam",        OPT_RCEXPANDPARAM,        false, false, false, false, true , false},
  {"rcquotes",             OPT_RCQUOTES,             false, false, false, false, true , false},
  {"rcs",                  OPT_RCS,                  true , true , true , true , false, true },
  {"recexact",             OPT_RECEXACT,             false, false, false, false, false, false},
  {"rematchpcre",          OPT_REMATCHPCRE,          false, false, false, false, false, false},
  {"restricted",           OPT_RESTRICTED,           false, false, false, false, false, true },
  {"rmstarsilent",         OPT_RMSTARSILENT,         false, true , true , false, false, false},
  {"rmstarwait",           OPT_RMSTARWAIT,           false, false, false, false, false, false},
  {"sharehistory",         OPT_SHAREHISTORY,         false, false, true , false, false, false},
  {"shfileexpansion",      OPT_SHFILEEXPANSION,      false, true , true , false, true , false},
  {"shglob",               OPT_SHGLOB,               false, true , true , false, true , false},
  {"shinstdin",            OPT_SHINSTDIN,            false, false, false, false, false, true },
  {"shnullcmd",            OPT_SHNULLCMD,            false, true , true , false, true , false},
  {"shoptionletters",      OPT_SHOPTIONLETTERS,      false, true , true , false, true , false},
  {"shortloops",           OPT_SHORTLOOPS,           true , false, false, true , true , false},
  {"shortrepeat",          OPT_SHORTREPEAT,          false, false, false, false, true , false},
  {"shwordsplit",          OPT_SHWORDSPLIT,          false, true , true , false, true , false},
  {"singlecommand",        OPT_SINGLECOMMAND,        false, false, false, false, false, true },
  {"singlelinezle",        OPT_SINGLELINEZLE,        false, false, true , false, false, false},
  {"sourcetrace",          OPT_SOURCETRACE,          false, false, false, false, false, false},
  {"sunkeyboardhack",      OPT_SUNKEYBOARDHACK,      false, false, false, false, false, false},
  {"transientrprompt",     OPT_TRANSIENTRPROMPT,     false, false, false, false, false, false},
  {"trapsasync",           OPT_TRAPSASYNC,           false, false, false, false, false, false},
  {"typesetsilent",        OPT_TYPESETSILENT,        false, true , true , false, true , false},
  {"typesettounset",       OPT_TYPESETTOUNSET,       false, true , true , false, true , false},
  {"unset",                OPT_UNSET,                true , true , true , false, true , false},
  {"verbose",              OPT_VERBOSE,              false, false, false, false, true , false},
  {"vi",                   OPT_VI,                   false, false, false, false, false, false},
  {"warncreateglobal",     OPT_WARNCREATEGLOBAL,     false, false, false, false, true , false},
  {"warnnestedvar",        OPT_WARNNESTEDVAR,        false, false, false, false, true , false},
  {"xtrace",               OPT_XTRACE,               false, false, false, false, true , false},
  {"zle",                  OPT_ZLE,                  false, false, false, false, false, true },
};

OptionState::OptionState() {
  for (const auto& info : OPTION_INFO)
    mOpts[info.index] = info.default_val;
}

static str normalize_option_name(strv name) {
  str s;
  s.reserve(name.size());
  for (char c : name) {
    if (c == '_') continue;
    s += static_cast<char>(s::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

optref<const OptionInfo> OptionState::find(strv name, bool* negated) noexcept {
  str norm = normalize_option_name(name);
  if (negated) *negated = false;
  for (const auto& info : OPTION_INFO)
    if (normalize_option_name(info.name) == norm) return info;
  if (norm.size() <= 2 or not norm.starts_with("no")) return s::nullopt;
  norm = norm.substr(2);
  for (const auto& info : OPTION_INFO) {
    if (normalize_option_name(info.name) == norm) {
      if (negated) *negated = true;
      return info;
    }
  }
  return s::nullopt;
}

bool OptionState::default_of(const OptionInfo& info) const noexcept {
  switch (mEmulation) {
    case Emulation::SH:  return info.emulation_sh;
    case Emulation::KSH: return info.emulation_ksh;
    case Emulation::CSH: return info.emulation_csh;
    default:             return info.default_val;
  }
}

void OptionState::apply_emulation(Emulation mode, bool full) {
  mEmulation = mode;
  for (const auto& info : OPTION_INFO)
    if (not info.special and (full or info.emulate)) mOpts[info.index] = default_of(info);
}

namespace {

struct Letter {
  char     c;
  OptIndex index;
  bool     negated;
};

// zsh's option letters (Src/options.c zshletters), in the order $- lists
// them.
constexpr Letter ZSH_LETTERS[] = {
  {'0', OPT_CORRECT,             false}, {'1', OPT_PRINTEXITVALUE,      false},
  {'2', OPT_BADPATTERN,          true},  {'3', OPT_NOMATCH,             true},
  {'4', OPT_GLOBDOTS,            false}, {'5', OPT_NOTIFY,              false},
  {'6', OPT_BGNICE,              false}, {'7', OPT_IGNOREEOF,           false},
  {'8', OPT_MARKDIRS,            false}, {'9', OPT_AUTOLIST,            false},
  {'B', OPT_BEEP,                true},  {'C', OPT_CLOBBER,             true},
  {'D', OPT_PUSHDTOHOME,         false}, {'E', OPT_PUSHDSILENT,         false},
  {'F', OPT_GLOB,                true},  {'G', OPT_NULLGLOB,            false},
  {'H', OPT_RMSTARSILENT,        false}, {'I', OPT_IGNOREBRACES,        false},
  {'J', OPT_AUTOCD,              false}, {'K', OPT_BANGHIST,            true},
  {'L', OPT_SUNKEYBOARDHACK,     false}, {'M', OPT_SINGLELINEZLE,       false},
  {'N', OPT_AUTOPUSHD,           false}, {'O', OPT_CORRECTALL,          false},
  {'P', OPT_RCEXPANDPARAM,       false}, {'Q', OPT_PATHDIRS,            false},
  {'R', OPT_LONGLISTJOBS,        false}, {'S', OPT_RECEXACT,            false},
  {'T', OPT_CDABLEVARS,          false}, {'U', OPT_MAILWARNING,         false},
  {'V', OPT_PROMPTCR,            true},  {'W', OPT_AUTORESUME,          false},
  {'X', OPT_LISTTYPES,           false}, {'Y', OPT_MENUCOMPLETE,        false},
  {'Z', OPT_ZLE,                 false}, {'a', OPT_ALLEXPORT,           false},
  {'d', OPT_GLOBALRCS,           true},  {'e', OPT_ERREXIT,             false},
  {'f', OPT_RCS,                 true},  {'g', OPT_HISTIGNORESPACE,     false},
  {'h', OPT_HISTIGNOREDUPS,      false}, {'i', OPT_INTERACTIVE,         false},
  {'k', OPT_INTERACTIVECOMMENTS, false}, {'l', OPT_LOGIN,               false},
  {'m', OPT_MONITOR,             false}, {'n', OPT_EXEC,                true},
  {'p', OPT_PRIVILEGED,          false}, {'r', OPT_RESTRICTED,          false},
  {'s', OPT_SHINSTDIN,           false}, {'t', OPT_SINGLECOMMAND,       false},
  {'u', OPT_UNSET,               true},  {'v', OPT_VERBOSE,             false},
  {'w', OPT_CHASELINKS,          false}, {'x', OPT_XTRACE,              false},
  {'y', OPT_SHWORDSPLIT,         false},
};

// The sh/ksh letters, used under SH_OPTION_LETTERS (kshletters).
constexpr Letter KSH_LETTERS[] = {
  {'C', OPT_CLOBBER,       true},  {'T', OPT_TRAPSASYNC,    false},
  {'X', OPT_MARKDIRS,      false}, {'a', OPT_ALLEXPORT,     false},
  {'b', OPT_NOTIFY,        false}, {'e', OPT_ERREXIT,       false},
  {'f', OPT_GLOB,          true},  {'i', OPT_INTERACTIVE,   false},
  {'l', OPT_LOGIN,         false}, {'m', OPT_MONITOR,       false},
  {'n', OPT_EXEC,          true},  {'p', OPT_PRIVILEGED,    false},
  {'r', OPT_RESTRICTED,    false}, {'s', OPT_SHINSTDIN,     false},
  {'t', OPT_SINGLECOMMAND, false}, {'u', OPT_UNSET,         true},
  {'v', OPT_VERBOSE,       false}, {'x', OPT_XTRACE,        false},
};

}  // namespace

opt<OptionLetter> OptionState::letter(char c) const noexcept {
  auto search = [c](const auto& table) -> opt<OptionLetter> {
    for (const auto& l : table)
      if (l.c == c) return OptionLetter{l.index, l.negated};
    return s::nullopt;
  };
  return mOpts[OPT_SHOPTIONLETTERS] ? search(KSH_LETTERS) : search(ZSH_LETTERS);
}

str OptionState::letters() const {
  str out;
  auto add = [&](const auto& table) {
    for (const auto& l : table)
      if (mOpts[l.index] != l.negated) out += l.c;
  };
  if (mOpts[OPT_SHOPTIONLETTERS]) add(KSH_LETTERS);
  else                            add(ZSH_LETTERS);
  return out;
}

}  // namespace jfsh
