#!/usr/bin/env python3
import re
import sys
import os

def fix_endif_comments(filepath):
    with open(filepath, 'r') as f:
        lines = f.readlines()
    
    if_stack = []
    modified = False
    new_lines = []
    
    for i, line in enumerate(lines):
        stripped = line.strip()
        
        # Match #if, #ifdef, #ifndef
        if_match = re.match(r'^#\s*(if|ifdef|ifndef)\s+(.+)$', stripped)
        if if_match:
            directive = if_match.group(1)
            condition = if_match.group(2).rstrip('\\').strip()
            if '//' in condition:
                condition = condition.split('//')[0].strip()
            if '/*' in condition:
                condition = condition.split('/*')[0].strip()
            if_stack.append((directive, condition))
            new_lines.append(line)
            continue
        
        if re.match(r'^#\s*else\b', stripped):
            new_lines.append(line)
            continue
        
        elif_match = re.match(r'^#\s*elif\s+(.+)$', stripped)
        if elif_match:
            new_lines.append(line)
            continue
        
        endif_match = re.match(r'^#\s*endif\s*(.*)$', stripped)
        if endif_match:
            existing_comment = endif_match.group(1).strip()
            if if_stack:
                directive, condition = if_stack.pop()
                # Only add/update comment if needed
                if not existing_comment.startswith('/*'):
                    indent = len(line) - len(line.lstrip())
                    indent_str = line[:indent]
                    
                    # Truncate long conditions
                    full_comment = f"#{directive} {condition}"
                    if len(full_comment) > 40:
                        # Shorten the comment
                        condition_short = condition[:35] + "..."
                        full_comment = f"#{directive} {condition_short}"
                    
                    new_line = f"{indent_str}#endif // {full_comment}\n"
                    if new_line != line:
                        modified = True
                    new_lines.append(new_line)
                else:
                    new_lines.append(line)
            else:
                new_lines.append(line)
            continue
        
        new_lines.append(line)
    
    if modified:
        with open(filepath, 'w') as f:
            f.writelines(new_lines)
        return True
    return False

if __name__ == '__main__':
    for filepath in sys.argv[1:]:
        if os.path.isfile(filepath):
            if fix_endif_comments(filepath):
                print(f"Fixed: {filepath}")
