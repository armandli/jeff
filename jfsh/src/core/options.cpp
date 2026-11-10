#include <jfsh/core/options.hh>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace jfsh {

// ---------------------------------------------------------------------------
// Option info table — one entry per OptIndex, alphabetical order.
// Fields: name, index, default_val, emulation_sh, emulation_ksh, emulation_csh
// ---------------------------------------------------------------------------
const OptionInfo kOptionInfo[OPT_COUNT] = {
    // A
    {"aliases",              OPT_ALIASES,              true,  false, true,  false},
    {"allexport",            OPT_ALLEXPORT,            false, false, false, false},
    {"alwayslastprompt",     OPT_ALWAYSLASTPROMPT,     true,  false, false, false},
    {"alwaystoend",          OPT_ALWAYSTOEND,          false, false, false, false},
    {"appendhistory",        OPT_APPENDHISTORY,        true,  false, false, false},
    {"autocd",               OPT_AUTOCD,               false, false, false, false},
    {"autocontinue",         OPT_AUTOCONTINUE,         false, false, false, false},
    {"autolist",             OPT_AUTOLIST,             true,  false, false, false},
    {"automenu",             OPT_AUTOMENU,             true,  false, false, false},
    {"autonamedirs",         OPT_AUTONAMEDIRS,         false, false, false, false},
    {"autoparamkeys",        OPT_AUTOPARAMKEYS,        true,  false, false, false},
    {"autoparamslash",       OPT_AUTOPARAMSLASH,       true,  false, false, false},
    {"autopushd",            OPT_AUTOPUSHD,            false, false, false, false},
    {"autoremoveslash",      OPT_AUTOREMOVESLASH,      true,  false, false, false},
    {"autoresume",           OPT_AUTORESUME,           false, false, false, false},
    // B
    {"bareglobqual",         OPT_BAREGLOBQUAL,         true,  false, false, false},
    {"beep",                 OPT_BEEP,                 true,  false, false, false},
    {"bgnice",               OPT_BGNICE,               false, false, false, false},
    {"braceccl",             OPT_BRACECCL,             false, false, false, false},
    {"bsdecho",              OPT_BSDECHO,              false, false, false, false},
    // C
    {"caseglob",             OPT_CASEGLOB,             true,  true,  true,  true},
    {"casematch",            OPT_CASEMATCH,            true,  false, false, false},
    {"cbases",               OPT_CBASES,               false, false, false, false},
    {"cdablevars",           OPT_CDABLEVARS,           false, false, false, false},
    {"chasedots",            OPT_CHASEDOTS,            false, false, false, false},
    {"chaselinks",           OPT_CHASELINKS,           false, false, false, false},
    {"checkjobs",            OPT_CHECKJOBS,            true,  false, false, false},
    {"checkrunningjobs",     OPT_CHECKRUNNINGJOBS,     true,  false, false, false},
    {"clobber",              OPT_CLOBBER,              true,  true,  true,  true},
    {"combiningchars",       OPT_COMBININGCHARS,       false, false, false, false},
    {"completealiases",      OPT_COMPLETEALIASES,      false, false, false, false},
    {"completeinword",       OPT_COMPLETEINWORD,       false, false, false, false},
    {"correct",              OPT_CORRECT,              false, false, false, false},
    {"correctall",           OPT_CORRECTALL,           false, false, false, false},
    {"cprecedences",         OPT_CPRECEDENCES,         false, false, false, false},
    {"cshjunkiehistory",     OPT_CSHJUNKIEHISTORY,     false, false, false, true},
    {"cshjunkieloops",       OPT_CSHJUNKIELOOPS,       false, false, false, true},
    {"cshjunkiequotes",      OPT_CSHJUNKIEQUOTES,      false, false, false, true},
    {"cshjoinctrls",         OPT_CSHJOINCTRLS,         false, false, false, false},
    {"cshnullcmd",           OPT_CSHNULLCMD,           false, false, false, true},
    {"cshnullglob",          OPT_CSHNULLGLOB,          false, false, false, false},
    // D
    {"debugbeforecmd",       OPT_DEBUGBEFORECMD,       false, false, false, false},
    {"dotglob",              OPT_DOTGLOB,              false, false, false, false},
    {"dvorak",               OPT_DVORAK,               false, false, false, false},
    // E
    {"emacs",                OPT_EMACS,                false, false, false, false},
    {"equals",               OPT_EQUALS,               true,  false, false, false},
    {"errexit",              OPT_ERREXIT,              false, true,  false, false},
    {"errreturn",            OPT_ERRRETURN,            false, false, false, false},
    {"evallineno",           OPT_EVALLINENO,           true,  false, false, false},
    {"exec",                 OPT_EXEC,                 true,  true,  true,  true},
    {"extendedglob",         OPT_EXTENDEDGLOB,         false, false, false, false},
    {"extendedhistory",      OPT_EXTENDEDHISTORY,      false, false, false, false},
    // F
    {"flowcontrol",          OPT_FLOWCONTROL,          false, false, false, false},
    {"forcefloat",           OPT_FORCEFLOAT,           false, false, false, false},
    {"functionargzero",      OPT_FUNCTIONARGZERO,      true,  false, false, false},
    // G
    {"globalexport",         OPT_GLOBALEXPORT,         true,  false, false, false},
    {"globalrcs",            OPT_GLOBALRCS,            true,  true,  true,  true},
    {"globassign",           OPT_GLOBASSIGN,           false, false, false, false},
    {"globcomplete",         OPT_GLOBCOMPLETE,         false, false, false, false},
    {"globdots",             OPT_GLOBDOTS,             false, false, false, false},
    {"globsubst",            OPT_GLOBSUBST,            false, true,  false, true},
    // H
    {"hashcmds",             OPT_HASHCMDS,             true,  false, false, false},
    {"hashdirs",             OPT_HASHDIRS,             true,  false, false, false},
    {"hashexecutablesonly",  OPT_HASHEXECUTABLESONLY,  false, false, false, false},
    {"histallowclobber",     OPT_HISTALLOWCLOBBER,     false, false, false, false},
    {"histbeep",             OPT_HISTBEEP,             true,  false, false, false},
    {"histexpiredupsfirst",  OPT_HISTEXPIREDUPSFIRST,  false, false, false, false},
    {"histfindnodups",       OPT_HISTFINDNODUPS,       false, false, false, false},
    {"histignorealldups",    OPT_HISTIGNOREALLDUPS,    false, false, false, false},
    {"histignoredups",       OPT_HISTIGNOREDUPS,       false, false, false, false},
    {"histignorespace",      OPT_HISTIGNORESPACE,      false, false, false, false},
    {"histlexwords",         OPT_HISTLEXWORDS,         false, false, false, false},
    {"histnodups",           OPT_HISTNODUPS,           false, false, false, false},
    {"histnostore",          OPT_HISTNOSTORE,          false, false, false, false},
    {"histreduceblanks",     OPT_HISTREDUCEBLANKS,     false, false, false, false},
    {"histsavebycopy",       OPT_HISTSAVEBYCOPY,       true,  false, false, false},
    {"histsavenodups",       OPT_HISTSAVENODUPS,       false, false, false, false},
    {"histsubstpattern",     OPT_HISTSUBSTPATTERN,     false, false, false, false},
    {"histverify",           OPT_HISTVERIFY,           false, false, false, false},
    {"hup",                  OPT_HUP,                  true,  false, false, false},
    // I
    {"ignorebraces",         OPT_IGNOREBRACES,         false, true,  false, false},
    {"ignoreclosebraces",    OPT_IGNORECLOSEBRACES,    false, false, false, false},
    {"ignoreeof",            OPT_IGNOREEOF,            false, false, false, false},
    // J K
    {"interactivecomments",  OPT_INTERACTIVECOMMENTS,  false, true,  true,  false},
    {"ksh_arrays",           OPT_KSH_ARRAYS,           false, false, true,  false},
    {"ksh_autoload",         OPT_KSH_AUTOLOAD,         false, false, true,  false},
    {"ksh_glob",             OPT_KSH_GLOB,             false, false, true,  false},
    {"ksh_option_print",     OPT_KSH_OPTION_PRINT,     false, false, true,  false},
    {"ksh_typeset",          OPT_KSH_TYPESET,          false, false, true,  false},
    {"ksh_zero_subscript",   OPT_KSH_ZERO_SUBSCRIPT,   false, false, false, false},
    // L
    {"listambiguous",        OPT_LISTAMBIGUOUS,        true,  false, false, false},
    {"listbeep",             OPT_LISTBEEP,             true,  false, false, false},
    {"listpacked",           OPT_LISTPACKED,           false, false, false, false},
    {"listrowsfirst",        OPT_LISTROWSFIRST,        false, false, false, false},
    {"listtypes",            OPT_LISTTYPES,            true,  false, false, false},
    {"localloops",           OPT_LOCALLOOPS,           false, false, false, false},
    {"localoptions",         OPT_LOCALOPTIONS,         false, false, false, false},
    {"localpatterns",        OPT_LOCALPATTERNS,        false, false, false, false},
    {"localtraps",           OPT_LOCALTRAPS,           false, false, false, false},
    {"logcheck",             OPT_LOGCHECK,             false, false, false, false},
    {"login",                OPT_LOGIN,                false, false, false, false},
    {"longlistjobs",         OPT_LONGLISTJOBS,         false, false, false, false},
    // M
    {"magicequalsubst",      OPT_MAGICEQUALSUBST,      false, false, false, false},
    {"mailwarn",             OPT_MAILWARN,             false, false, false, false},
    {"markdirs",             OPT_MARKDIRS,             false, false, false, false},
    {"menucomplete",         OPT_MENUCOMPLETE,         false, false, false, false},
    {"monitor",              OPT_MONITOR,              true,  false, false, false},
    {"multibyte",            OPT_MULTIBYTE,            true,  true,  true,  true},
    {"multifuncdef",         OPT_MULTIFUNCDEF,         true,  false, false, false},
    {"multios",              OPT_MULTIOS,              true,  false, false, false},
    // N
    {"nomatch",              OPT_NOMATCH,              true,  true,  true,  false},
    {"notify",               OPT_NOTIFY,               false, false, false, false},
    {"nullglob",             OPT_NULLGLOB,             false, false, false, false},
    {"numericglobsort",      OPT_NUMERICGLOBSORT,      false, false, false, false},
    // O
    {"octalzeroes",          OPT_OCTALZEROES,          false, false, false, false},
    {"overstrike",           OPT_OVERSTRIKE,           false, false, false, false},
    // P
    {"pathdirs",             OPT_PATHDIRS,             false, false, false, false},
    {"pathscript",           OPT_PATHSCRIPT,           false, false, false, false},
    {"pipefail",             OPT_PIPEFAIL,             false, false, false, false},
    {"posixaliases",         OPT_POSIXALIASES,         false, false, false, false},
    {"posixargzero",         OPT_POSIXARGZERO,         false, false, false, false},
    {"posixbuiltins",        OPT_POSIXBUILTINS,        false, false, false, false},
    {"posixcd",              OPT_POSIXCD,              false, false, false, false},
    {"posixidentifiers",     OPT_POSIXIDENTIFIERS,     false, false, false, false},
    {"posixjobs",            OPT_POSIXJOBS,            false, false, false, false},
    {"posixstrings",         OPT_POSIXSTRINGS,         false, false, false, false},
    {"posixtraps",           OPT_POSIXTRAPS,           false, false, false, false},
    {"printeightbit",        OPT_PRINTEIGHTBIT,        false, false, false, false},
    {"printexitvalue",       OPT_PRINTEXITVALUE,       false, false, false, false},
    {"privileged",           OPT_PRIVILEGED,           false, false, false, false},
    {"promptbang",           OPT_PROMPTBANG,           false, false, false, true},
    {"promptcr",             OPT_PROMPTCR,             true,  false, false, false},
    {"promptpercent",        OPT_PROMPTPERCENT,        true,  false, false, false},
    {"promptsp",             OPT_PROMPTSP,             true,  false, false, false},
    {"promptsubst",          OPT_PROMPTSUBST,          false, false, false, false},
    {"pushdignoredups",      OPT_PUSHDIGNOREDUPS,      false, false, false, false},
    {"pushdminus",           OPT_PUSHDMINUS,           false, false, false, false},
    {"pushdsilent",          OPT_PUSHDSILENT,          false, false, false, false},
    {"pushdtohome",          OPT_PUSHDTOHOME,          false, false, false, false},
    // R
    {"rcexpandparam",        OPT_RCEXPANDPARAM,        false, false, false, false},
    {"rcquotes",             OPT_RCQUOTES,             false, false, false, false},
    {"rcs",                  OPT_RCS,                  true,  true,  true,  true},
    {"recexact",             OPT_RECEXACT,             false, false, false, false},
    {"rematchpcre",          OPT_REMATCHPCRE,          false, false, false, false},
    {"restricted",           OPT_RESTRICTED,           false, false, false, false},
    {"rmstarsilent",         OPT_RMSTARSILENT,         false, false, false, false},
    {"rmstarwait",           OPT_RMSTARWAIT,           false, false, false, false},
    // S
    {"sharehistory",         OPT_SHAREHISTORY,         false, false, false, false},
    {"shfileexpansion",      OPT_SHFILEEXPANSION,      false, false, false, false},
    {"shglob",               OPT_SHGLOB,               false, false, false, false},
    {"shinstdin",            OPT_SHINSTDIN,            false, false, false, false},
    {"shnullcmd",            OPT_SHNULLCMD,            true,  true,  false, false},
    {"shoptionletters",      OPT_SHOPTIONLETTERS,      false, false, false, false},
    {"shortloops",           OPT_SHORTLOOPS,           true,  false, false, false},
    {"shortrepeat",          OPT_SHORTREPEAT,          false, false, false, false},
    {"shwordsplit",          OPT_SHWORDSPLIT,          false, true,  false, true},
    {"singlecommand",        OPT_SINGLECOMMAND,        false, false, false, false},
    {"singlelinezle",        OPT_SINGLELINEZLE,        false, false, false, false},
    {"sourcetrace",          OPT_SOURCETRACE,          false, false, false, false},
    {"stdin",                OPT_STDIN,                false, false, false, false},
    {"sunkeyboardhack",      OPT_SUNKEYBOARDHACK,      false, false, false, false},
    // T
    {"transientrprompt",     OPT_TRANSIENTRPROMPT,     false, false, false, false},
    {"trapsasync",           OPT_TRAPSASYNC,           false, false, false, false},
    {"typesetsilent",        OPT_TYPESETSILENT,        false, false, false, false},
    // U
    {"unset",                OPT_UNSET,                true,  true,  false, true},
    {"verbose",              OPT_VERBOSE,              false, false, false, false},
    // V
    {"vi",                   OPT_VI,                   false, false, false, false},
    // W
    {"warncreateglobal",     OPT_WARNCREATEGLOBAL,     false, false, false, false},
    {"warnnestedvar",        OPT_WARNNESTEDVAR,        false, false, false, false},
    // X
    {"xtrace",               OPT_XTRACE,               false, false, false, false},
    // Z
    {"zlebindings",          OPT_ZLEBINDINGS,          false, false, false, false},
};

// ---------------------------------------------------------------------------
// OptionState
// ---------------------------------------------------------------------------

OptionState::OptionState() {
    for (const auto& info : kOptionInfo)
        opts_[info.index] = info.default_val;
}

static std::string normalize_option_name(std::string_view name) {
    std::string s;
    s.reserve(name.size());
    for (char c : name) {
        if (c == '_') continue;  // strip underscores
        s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

const OptionInfo* OptionState::find(std::string_view name) noexcept {
    // Strip leading "no" prefix if present to handle "noextendedglob" etc.
    std::string norm = normalize_option_name(name);
    bool negated = false;
    if (norm.size() > 2 && norm.substr(0, 2) == "no") {
        // try with "no" stripped
        negated = true;
        norm = norm.substr(2);
    }
    (void)negated;  // used by callers who check the negated flag separately

    for (const auto& info : kOptionInfo) {
        if (normalize_option_name(info.name) == norm)
            return &info;
    }
    return nullptr;
}

void OptionState::apply_emulation(Emulation mode) {
    for (const auto& info : kOptionInfo) {
        bool val = info.default_val;
        switch (mode) {
            case Emulation::ZSH: val = info.default_val;   break;
            case Emulation::SH:  val = info.emulation_sh;  break;
            case Emulation::KSH: val = info.emulation_ksh; break;
            case Emulation::CSH: val = info.emulation_csh; break;
        }
        opts_[info.index] = val;
    }
}

} // namespace jfsh
