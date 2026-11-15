"""Method extraction from the lemonade display test helper; see NOTICE."""
import re

def method(source, name):
    match = re.search(r'^(?:static )?\w+\s*\n' + re.escape(name) + r'\([^;]*?\)\s*\{', source, re.M)
    if match is None:
        raise ValueError('Missing method: ' + name)
    start = source.index('{', match.start())
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                    lambda m: ' ' * len(m[0]), source, flags=re.S)
    depth = 0
    for end in range(start, len(source)):
        depth += (masked[end] == '{') - (masked[end] == '}')
        if not depth:
            return source[match.start():end + 1]
    raise ValueError('Unclosed method: ' + name)
