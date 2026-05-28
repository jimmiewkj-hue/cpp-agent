from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
log_path = Path(r"g:\downloads\claude-code\yuanma-poxi\cpp-agent\.dbg\runner-closure-debug.ndjson")
log_path.parent.mkdir(parents=True, exist_ok=True)
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/health':
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b'ok')
            return
        self.send_response(404)
        self.end_headers()
    def do_POST(self):
        length = int(self.headers.get('Content-Length', '0'))
        body = self.rfile.read(length).decode('utf-8', errors='replace')
        with log_path.open('a', encoding='utf-8') as f:
            f.write(body)
            f.write('\n')
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b'ok')
    def log_message(self, format, *args):
        return
HTTPServer(('127.0.0.1', 7777), Handler).serve_forever()
