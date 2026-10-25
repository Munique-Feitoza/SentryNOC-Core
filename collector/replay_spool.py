#!/usr/bin/env python3
"""Replay a local NDJSON spool into the collector after a connectivity outage."""
import argparse
import json
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument("spool")
parser.add_argument("--url", default="http://127.0.0.1:8787/api/v1/ingest")
parser.add_argument("--token", default="")
args = parser.parse_args()

sent = 0
with open(args.spool, encoding="utf-8") as source:
    for line in source:
        if not line.strip():
            continue
        request = urllib.request.Request(args.url, data=(line.strip() + "\n").encode(), method="POST")
        request.add_header("Content-Type", "application/x-ndjson")
        if args.token:
            request.add_header("Authorization", "Bearer " + args.token)
        with urllib.request.urlopen(request, timeout=10) as response:
            if response.status not in (200, 202):
                raise RuntimeError("collector returned %s" % response.status)
        sent += 1
print("replayed %d samples" % sent)
