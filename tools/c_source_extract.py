"""Extract C function bodies for executable host regressions."""
import re
def function(source, name):
    uncommented = re.sub('//[^\\n]*|/\\*[\\s\\S]*?\\*/', lambda m: re.sub('[^\\n]', ' ', m[0]), source)
    match = re.search('(?m)^(?:static\\s+)?[\\w\\s*]+\\b' + re.escape(name) + '\\([^;{]*\\)\\s*\\{', uncommented)
    if not match:
        raise RuntimeError(name)
    start = match.start()
    while source[start].isspace():
        start += 1
    brace = uncommented.index('{', match.start())
    depth = 0
    token = re.compile('//[^\\n]*|/\\*[\\s\\S]*?\\*/|"(?:\\\\.|[^"\\\\])*"|\\\'(?:\\\\.|[^\\\'\\\\])*\\\'|[{}]')
    for m in token.finditer(source, brace):
        if m[0] == '{':
            depth += 1
        elif m[0] == '}':
            depth -= 1
            if depth == 0:
                return source[start:m.end()].strip() + '\n'
    raise RuntimeError('unclosed ' + name)
