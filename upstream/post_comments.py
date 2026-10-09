#!/usr/bin/env python3
"""Publica comentarios en hn/ginlong-solis. Requiere token fine-grained con
'Issues: Read and write' sobre hn/ginlong-solis (o classic con scope public_repo).

Uso:  python3 post_comments.py            # publica los .md de este directorio
      python3 post_comments.py --dry-run  # solo muestra qué haría
"""
import json, os, sys, urllib.request, urllib.error
from pathlib import Path

TOK = (os.environ.get("GH_TOKEN") or Path.home().joinpath(".gh_token").read_text()).strip()
API = "https://api.github.com/repos/hn/ginlong-solis/issues/{n}/comments"
HERE = Path(__file__).parent
DRY = "--dry-run" in sys.argv

# issue -> fichero con el cuerpo
TARGETS = {76: "comment-76.md", 72: "comment-72.md"}


def post(number, body):
    if DRY:
        print(f"[dry-run] #{number}: {len(body)} chars\n{body[:200]}...\n")
        return
    data = json.dumps({"body": body}).encode()
    req = urllib.request.Request(API.format(n=number), data=data, method="POST",
                                 headers={"Authorization": "Bearer " + TOK,
                                          "Accept": "application/vnd.github+json",
                                          "Content-Type": "application/json"})
    try:
        r = json.load(urllib.request.urlopen(req, timeout=30))
        print(f"OK #{number} -> {r['html_url']}")
    except urllib.error.HTTPError as e:
        print(f"ERR #{number}: {e.code} {e.read().decode()[:300]}")


for n, f in TARGETS.items():
    post(n, (HERE / f).read_text().strip())
