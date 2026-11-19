import re
import sys
from pathlib import Path


def generate(source):
    result = []
    for number, line in enumerate(source.splitlines(), 1):
        line = line.split('#', 1)[0].strip()
        if not line or line in ('type yukizygisk_file file_type',
                               'typeattribute yukizygisk_file mlstrustedobject'):
            continue
        match = re.fullmatch(r'allow ([\w$]+) ([\w$]+) (\w+) (\w+|\{[\w ]+\})', line)
        if not match:
            raise ValueError(f'unsupported policy at line {number}')
        source_type, target, cls, permissions = match.groups()
        for permission in permissions.strip('{} ').split():
            result.append(f'{{ "{source_type}", "{target}", "{cls}", "{permission}" }},')
    return '\n'.join(result) + '\n'


if __name__ == '__main__':
    contents = generate(Path(sys.argv[1]).read_text())
    output = Path(sys.argv[2])
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + '.tmp')
    temporary.write_text(contents, encoding='utf-8')
    temporary.replace(output)
