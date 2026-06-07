"""End-to-end test for the 5-level fuzzy matching fallback (de-sanitize removed)."""
import sys

DESANITIZE_REMOVED = True  # cpp-agent runs independently, no API sanitization

def normalize_crlf(s):
    return s.replace('\r\n', '\n').replace('\r', '\n')

def normalize_quotes(s):
    for curly, straight in [('\u2018', "'"), ('\u2019', "'"), ('\u201c', '"'), ('\u201d', '"')]:
        s = s.replace(curly, straight)
    return s

def strip_trailing_whitespace(s):
    return '\n'.join(line.rstrip() for line in s.split('\n'))

def is_markdown(path):
    return path.lower().endswith(('.md', '.mdx'))

def find_actual_string(file_content, old_string, file_path="test.py"):
    """Simulates the 5-level fallback (de-sanitize removed)."""
    md = is_markdown(file_path)

    # Step 1: Exact match
    if old_string in file_content:
        return old_string, 'exact'

    # Step 2: CRLF normalization
    norm_content = normalize_crlf(file_content)
    norm_old = normalize_crlf(old_string)
    if norm_old in norm_content:
        return None, 'CRLF-normalization'

    # Step 3: Quote + CRLF normalization
    norm_content3 = normalize_crlf(normalize_quotes(file_content))
    norm_old3 = normalize_crlf(normalize_quotes(old_string))
    if norm_old3 in norm_content3:
        return None, 'quote+CRLF-normalization'

    # Step 4: Whitespace + CRLF (skip for .md/.mdx)
    if not md:
        stripped_content = normalize_crlf(strip_trailing_whitespace(file_content))
        stripped_old = normalize_crlf(strip_trailing_whitespace(old_string))
        if stripped_old in stripped_content:
            return None, 'whitespace+CRLF-normalization'

    # Step 5: Full normalization
    norm_old5 = normalize_quotes(old_string)
    if not md:
        norm_old5 = strip_trailing_whitespace(norm_old5)
    norm_old5 = normalize_crlf(norm_old5)

    norm_content5 = normalize_quotes(file_content)
    if not md:
        norm_content5 = strip_trailing_whitespace(norm_content5)
    norm_content5 = normalize_crlf(norm_content5)

    if norm_old5 in norm_content5:
        return None, 'full-normalization'

    return None, 'not-found'


passed = 0
failed = 0

def test(name, file_content, old_string, expected_method, file_path="test.py"):
    global passed, failed
    result, method = find_actual_string(file_content, old_string, file_path)
    status = "PASS" if method == expected_method else "FAIL"
    if status == "PASS":
        passed += 1
    else:
        failed += 1
    print(f"  [{status}] {name}: expected={expected_method}, got={method}")

print("=== Fuzzy Matching 5-Level Fallback (de-sanitize removed) ===\n")

# Step 1: Exact match
test("Exact match", "hello world\nfoo bar\n", "foo bar", "exact")

# Step 2: CRLF normalization
test("CRLF mismatch (file=CRLF, search=LF)", "hello\r\nfoo bar\r\n", "foo bar\n", "CRLF-normalization")
test("CRLF mismatch (file=LF, search=CRLF)", "hello\nfoo bar\n", "foo bar\r\n", "CRLF-normalization")

# Step 3: Quote + CRLF normalization
test("Curly quotes + CRLF", "msg = \u201chello\u201d\r\n", 'msg = "hello"\n', "quote+CRLF-normalization")
test("Curly single quotes", "msg = \u2018hi\u2019\n", "msg = 'hi'", "quote+CRLF-normalization")

# Step 4: Whitespace + CRLF (not for .md)
test("Trailing whitespace + CRLF", "foo = bar   \r\nbaz = qux\r\n", "foo = bar\nbaz = qux\n", "whitespace+CRLF-normalization")

# Step 4 skipped for .md files
test("Markdown: skip whitespace strip", "foo = bar   \n", "foo = bar   \n", "exact", "test.md")

# Step 5: Full normalization
test("Full: curly+whitespace+CRLF", "msg = \u201chello\u201d   \r\n", 'msg = "hello"\n', "full-normalization")

# De-sanitize is REMOVED - verify it doesn't match
test("De-sanitize REMOVED: no match", "result = <function_results>ok</function_results>\n", "result = <fnr>ok</fnr>", "not-found")

# Not found
test("Not found", "hello world\n", "xyz not in file", "not-found")

print(f"\n=== Results: {passed} passed, {failed} failed ===")
if failed > 0:
    sys.exit(1)