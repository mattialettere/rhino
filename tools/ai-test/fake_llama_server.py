#!/usr/bin/env python3
"""Server di prova al posto di llama-server, per provare senza modello il lavoro per parti
dell'assistente (piano, parti, correzioni). Risponde con l'esempio della libreria che
compare nelle istruzioni per la richiesta: e' la risposta "ideale" di un modello.

  python3 fake_llama_server.py 8099 [--break]

--break: la prima risposta per la seconda parte e' sbagliata (comando inesistente) e
la prima per la terza parte e' nel posto sbagliato, per provare le correzioni.
Solo libreria standard di Python.
"""
import http.server
import json
import re
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8099
BREAK = "--break" in sys.argv
attempts = {}


def example_blocks(system, request):
    """Piano e parti dell'esempio con quella richiesta, dal testo delle istruzioni."""
    start = system.find('Example request: "%s"' % request)
    if start < 0:
        start = system.find("Example request:")
    end = system.find("Example request:", start + 1)
    chunk = system[start:end if end > 0 else len(system)]
    plan = re.search(r"```plan\n(.*?)```", chunk, re.S)
    parts = dict(re.findall(r"Part (\w+):\n```kurva\n(.*?)```", chunk, re.S))
    return (plan.group(1) if plan else ""), parts


def answer(body):
    msgs = body["messages"]
    system = msgs[0]["content"]
    first_user = next(m["content"] for m in msgs if m["role"] == "user")
    request = first_user.split("\n\nStep 1")[0].strip()
    last = msgs[-1]["content"]
    plan, parts = example_blocks(system, request)
    grammar = body.get("grammar", "")
    if grammar.startswith('root ::= "```plan') or "Step 1" in last or "plan is not valid" in last:
        return "```plan\n" + plan + "```"
    m = re.search(r"Part (\d+) of \d+: (\w+)", last)
    if not m:  # correzione: la parte e' quella dell'ultima richiesta di parte
        for msg in reversed(msgs):
            m = re.search(r"Part (\d+) of \d+: (\w+)", msg["content"]) if msg["role"] == "user" else None
            if m:
                break
    if not m:
        return "```kurva\n_Box 0,0,0 10,10 10\n```"
    k, name = int(m.group(1)), m.group(2)
    attempts[(request, k)] = attempts.get((request, k), 0) + 1
    script = parts.get(name, "_Box 0,0,0 10,10 10 => %s\n" % name)
    if BREAK and attempts[(request, k)] == 1:
        if k == 2:
            return "```kurva\n_Volare 1,2,3\n```"
        if k == 3:  # tutto spostato di 5 metri lungo X
            moved = re.sub(r"(?m)^(_\w+)", r"\1", script)
            return "```kurva\n" + moved + "_Move 0,0,0 5000,0,0 @%s\n```" % name
    return "```kurva\n" + script + "```"


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, obj):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self._send(200, {"status": "ok"})

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        text = answer(body)
        self._send(200, {"choices": [{"message": {"role": "assistant", "content": text}}]})


http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
