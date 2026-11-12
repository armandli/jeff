#include <jfsh/glob/glob.h>

// Extended-glob pattern compiler (requires EXTENDED_GLOB option).
//
// TODO(glob/extglob): Implement zsh extended glob operators:
//   #    zero or more occurrences of previous pattern (like * in regex)
//   ##   one or more occurrences (like + in regex)
//   ^    negate: matches anything not matching the following pattern
//   ~    exception: matches left side except what matches right side
//   (pat1|pat2)  alternation group
//
// TODO(glob/recurse): Implement **/pattern recursive glob (GLOB_RECURSE flag).
//   Walk the directory tree depth-first; match filename against the final
//   component pattern, include both files and directories as appropriate.
//
// TODO(glob/quals): Implement zsh glob qualifiers inside parentheses.
//   Parse the GlobQualifier list from the GlobPattern.quals vector.
//   Apply each qualifier as a stat(2) filter on candidate matches.
//   Sorting qualifiers: n (name), L (size), m (mtime), a (atime), c (ctime).
//   Ordering qualifiers: o (ascending), O (descending).
//
// TODO(glob/numeric): Implement NUMERIC_GLOB_SORT option — when set, sort
//   match results numerically rather than lexicographically.
//
// TODO(glob/mark): Implement the MARK_DIRS option — append / to matched dirs.

namespace jfsh {
}  // namespace jfsh
