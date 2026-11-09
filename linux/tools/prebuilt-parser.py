#!/usr/bin/env python3
"""prebuilt-parser.py — stands in for bison/flex in the kernel build.

This machine has neither bison nor flex.  Kbuild needs them only for kconfig
(scripts/kconfig/parser.y, lexer.l) as long as DTC, genksyms (MODVERSIONS)
and friends are off.  The Ubuntu kernel headers package ships the generated
parser.tab.[ch] and lexer.lex.c for kconfig; the grammar of v7.3.3 is the
same as theirs and only some C action code differs, so this script copies
those files and applies the v7.3.3 action-code changes, which is what
regenerating them would give (#line numbers aside).

Kbuild calls it as $(YACC)/$(LEX):
  prebuilt-parser.py -o X.tab.c --defines=X.tab.h -t -l <src>/parser.y
  prebuilt-parser.py -oX.lex.c -L <src>/lexer.l
Anything else is refused with an error naming the missing tool.
"""
import glob, os, sys

def headers_dir():
    cands = sorted(glob.glob('/usr/src/linux-headers-*/scripts/kconfig/parser.tab.c'))
    if not cands:
        sys.exit('prebuilt-parser.py: no /usr/src/linux-headers-*/scripts/kconfig '
                 'generated parser; install bison and flex instead')
    return os.path.dirname(cands[-1])

# (old, new) pairs: the kconfig action code that differs between the
# headers package's sources and v7.3.3's, as v7.3.3 has it.
PARSER_EDITS = [
    ("""		/*
		 * If the same symbol appears twice in a choice block, the list
		 * node would be added twice, leading to a broken linked list.
		 * list_empty() ensures that this symbol has not yet added.
		 */
		if (list_empty(&current_entry->sym->choice_link))
			list_add_tail(&current_entry->sym->choice_link,
				      &current_choice->choice_members);""",
     """		list_add_tail(&current_entry->sym->choice_link,
			      &current_choice->choice_members);"""),
    ("""		if (prop->menu != menu && prop->type == P_PROMPT &&
		    prop->menu->parent != menu->parent) {""",
     """		if (prop->menu != menu && prop->type == P_PROMPT) {"""),
    ('"choice value has a prompt outside its choice group\\n");',
     '"choice value must not have a prompt in another entry\\n");'),
]
LEXER_EDITS = [  # in order: zconf_initscan, then zconf_nextfile
    ("cur_filename = file_lookup(name);", "cur_filename = file_lookup(name, NULL, 0);"),
    ("cur_filename = file_lookup(name);", "cur_filename = file_lookup(name, cur_filename, cur_lineno);"),
]

def edit(text, edits, what):
    for old, new in edits:
        if old not in text:
            sys.exit(f'prebuilt-parser.py: {what}: expected text not found; '
                     'the headers package or the kernel version changed, regenerate with bison/flex')
        text = text.replace(old, new, 1)
    return text

def main(argv):
    src = argv[-1]
    base = os.path.basename(src)
    hd = headers_dir()
    if '--defines' in ' '.join(argv) and base == 'parser.y':
        out_c = argv[argv.index('-o') + 1]
        out_h = [a for a in argv if a.startswith('--defines=')][0].split('=', 1)[1]
        c = edit(open(os.path.join(hd, 'parser.tab.c')).read(), PARSER_EDITS, 'parser.tab.c')
        open(out_c, 'w').write(c)
        open(out_h, 'w').write(open(os.path.join(hd, 'parser.tab.h')).read())
        return
    if base == 'lexer.l':
        out = [a[2:] for a in argv if a.startswith('-o')][0]
        l = edit(open(os.path.join(hd, 'lexer.lex.c')).read(), LEXER_EDITS, 'lexer.lex.c')
        open(out, 'w').write(l)
        return
    sys.exit(f'prebuilt-parser.py: no prebuilt output for {src}: install bison/flex '
             '(or keep the option that needs it off)')

main(sys.argv[1:])
