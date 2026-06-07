# Simulate the fuzzy matching logic in Python to verify correctness

def normalize_crlf(s):
    return s.replace('\r\n', '\n').replace('\r', '\n')

def normalize_quotes(s):
    for curly, straight in [('\u2018', "'"), ('\u2019', "'"), ('\u201c', '"'), ('\u201d', '"')]:
        s = s.replace(curly, straight)
    return s

def strip_trailing_whitespace(s):
    lines = s.split('\n')
    return '\n'.join(line.rstrip() for line in lines)

def find_actual_string(file_content, old_string):
    # Step 1: Exact match
    if old_string in file_content:
        return old_string, 'exact'

    # Step 2: CRLF normalization
    norm_content = normalize_crlf(file_content)
    norm_old = normalize_crlf(old_string)
    if norm_old in norm_content:
        # Map back to original
        pos = norm_content.find(norm_old)
        orig_pos = 0
        norm_pos = 0
        while norm_pos < pos:
            if file_content[orig_pos:orig_pos+2] == '\r\n':
                orig_pos += 2
                norm_pos += 1
            else:
                orig_pos += 1
                norm_pos += 1
        start = orig_pos
        chars_remaining = len(norm_old)
        while chars_remaining > 0:
            if file_content[orig_pos:orig_pos+2] == '\r\n':
                orig_pos += 2
                chars_remaining -= 1
            else:
                orig_pos += 1
                chars_remaining -= 1
        return file_content[start:orig_pos], 'CRLF-normalization'

    # Step 3: Quote + CRLF normalization
    norm_content2 = normalize_crlf(normalize_quotes(file_content))
    norm_old2 = normalize_crlf(normalize_quotes(old_string))
    if norm_old2 in norm_content2:
        return None, 'quote+CRLF-normalization'

    # Step 4: Whitespace + CRLF normalization
    stripped_content = normalize_crlf(strip_trailing_whitespace(file_content))
    stripped_old = normalize_crlf(strip_trailing_whitespace(old_string))
    if stripped_old in stripped_content:
        return None, 'whitespace+CRLF-normalization'

    return None, 'not-found'

# Test cases
print('=== Test 1: Exact match ===')
result, method = find_actual_string('hello world\nfoo bar\n', 'foo bar')
print(f'Result: {repr(result)}, Method: {method}')
assert method == 'exact'

print()
print('=== Test 2: CRLF mismatch (file has CRLF, search has LF) ===')
result, method = find_actual_string('hello world\r\nfoo bar\r\n', 'foo bar\n')
print(f'Result: {repr(result)}, Method: {method}')
assert method == 'CRLF-normalization'
assert result == 'foo bar\r\n'

print()
print('=== Test 3: CRLF mismatch (file has LF, search has CRLF) ===')
result, method = find_actual_string('hello world\nfoo bar\n', 'foo bar\r\n')
print(f'Result: {repr(result)}, Method: {method}')
assert method == 'CRLF-normalization'

print()
print('=== Test 4: Curly quotes ===')
result, method = find_actual_string('msg = \u201chello\u201d\n', 'msg = "hello"')
print(f'Result: {repr(result)}, Method: {method}')
assert method == 'quote+CRLF-normalization'

print()
print('=== Test 5: Trailing whitespace ===')
result, method = find_actual_string('foo = bar   \nbaz = qux\n', 'foo = bar\n')
print(f'Result: {repr(result)}, Method: {method}')
assert method == 'whitespace+CRLF-normalization'

print()
print('=== Test 6: Not found ===')
result, method = find_actual_string('hello world\n', 'xyz not in file')
print(f'Result: {repr(result)}, Method: {method}')
assert method == 'not-found'

print()
print('All tests passed!')
