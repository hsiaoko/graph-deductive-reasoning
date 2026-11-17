#!/usr/bin/env python3
import argparse
import json
from urllib.request import Request, urlopen


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://127.0.0.1:30000")
    parser.add_argument("--model", default="local-model")
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()
    body = {
        "model": args.model,
        "state": "The customer requests a refund.",
        "questions": {
            "route": {
                "type": "choice",
                "instructions": "Choose the appropriate action.",
                "criteria": {
                    "approve": "Approve the refund",
                    "reject": "Reject the refund",
                },
            }
        },
    }
    request = Request(
        args.base_url.rstrip("/") + "/v1/systemone",
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urlopen(request, timeout=args.timeout) as response:
        payload = json.load(response)
    print(json.dumps(payload, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
