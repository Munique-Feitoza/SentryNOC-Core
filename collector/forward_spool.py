#!/usr/bin/env python3
"""Continuously forwards new NDJSON lines from the agent spool to the collector."""
import argparse
import os
import time
import urllib.error
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument("--spool", default="/var/lib/sentrynoc/telemetry.ndjson")
parser.add_argument("--url", default="http://127.0.0.1:8787/api/v1/ingest")
parser.add_argument("--token", default=os.getenv("SENTRYNOC_TOKEN", ""))
parser.add_argument("--offset", default="/var/lib/sentrynoc/telemetry.offset")
args = parser.parse_args()

try:
    position = int(open(args.offset, encoding="utf-8").read())
except (FileNotFoundError, ValueError):
    position = 0

while True:
    try:
        with open(args.spool, "r", encoding="utf-8") as source:
            source.seek(position)
            while True:
                line = source.readline()
                if not line:
                    break
                request = urllib.request.Request(args.url, data=line.encode(), method="POST")
                request.add_header("Content-Type", "application/x-ndjson")
                if args.token:
                    request.add_header("Authorization", "Bearer " + args.token)
                with urllib.request.urlopen(request, timeout=10) as response:
                    if response.status not in (200, 202):
                        raise RuntimeError("collector returned %s" % response.status)
                position = source.tell()
                with open(args.offset, "w", encoding="utf-8") as marker:
                    marker.write(str(position))
    except (FileNotFoundError, OSError, urllib.error.URLError, RuntimeError) as error:
        print("forwarder waiting: %s" % error)
        time.sleep(3)
    time.sleep(1)
