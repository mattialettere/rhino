#!/usr/bin/env python3
"""Server di prova al posto di llama-server, per provare senza modello il lavoro per parti
dell'assistente (piano, parti, correzioni). Risponde con il piano d'esempio dello stesso
oggetto mostrato nella richiesta e, per ogni parte, con la stessa parte dell'esempio
mostrata nella richiesta della parte: e' la risposta "ideale" di un modello.

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


def same_plan(first_user):
    """Piano dell'esempio dello stesso oggetto, dalla prima richiesta."""
    i = first_user.find("Plan of the same kind of object")
    m = re.search(r"```plan\n(.*?)```", first_user[i:], re.S) if i >= 0 else None
    return m.group(1) if m else ""


def same_part(text):
    """Comandi della stessa parte dell'esempio, dalla richiesta della parte."""
    i = text.find("The same part in the example of this object")
    m = re.search(r"```kurva\n(.*?)```", text[i:], re.S) if i >= 0 else None
    return m.group(1) if m else None


def answer(body):
    msgs = body["messages"]
    first_user = next(m["content"] for m in msgs if m["role"] == "user")
    m = re.search(r"Request: (.*?)\n\nStep 1", first_user, re.S)
    request = m.group(1).strip() if m else first_user
    last = msgs[-1]["content"]
    grammar = body.get("grammar", "")
    if grammar.startswith('root ::= "```plan') or "Step 1" in last or "plan is not valid" in last:
        return "```plan\n" + same_plan(first_user) + "```"
    m = re.search(r"Part (\d+) of \d+: (\w+)", last)
    part_msg = last
    if not m:  # correzione: la parte e' quella dell'ultima richiesta di parte
        for msg in reversed(msgs):
            m = re.search(r"Part (\d+) of \d+: (\w+)", msg["content"]) if msg["role"] == "user" else None
            if m:
                part_msg = msg["content"]
                break
    if not m:
        return "```kurva\n_Box 0,0,0 10,10 10\n```"
    k, name = int(m.group(1)), m.group(2)
    attempts[(request, k)] = attempts.get((request, k), 0) + 1
    script = same_part(part_msg) or "_Box 0,0,0 10,10 10 => %s\n" % name
    if BREAK and attempts[(request, k)] == 1:
        if k == 2:
            return "```kurva\n_Volare 1,2,3\n```"
        if k == 3:  # tutto spostato di 5 metri lungo X
            return "```kurva\n" + script + "_Move 0,0,0 5000,0,0 @%s\n```" % name
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
