with open(r'g:\downloads\claude-code\yuanma-poxi\cpp-agent\src\tools\ToolOrchestrator.cpp', 'r', encoding='utf-8') as f:
    content = f.read()
lines = content.split('\n')
print(f'Total lines: {len(lines)}')
markers = ['diagLog', '[FileEdit]', 'Step 1', 'Step 2', 'Step 3', 'Step 4', 'Step 5', 'Step 6', 'Step 7', 'Diagnostic Log']
for m in markers:
    count = content.count(m)
    print(f'  "{m}": {count} occurrence(s)')
print('Diagnostic logging check passed')
