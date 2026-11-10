#!/usr/bin/env python3
"""Train and evaluate NBFNet or A*Net on standard KG TSV splits."""

import argparse
import json
from pathlib import Path
import time

import torch

from .engine import evaluate, resolve_device, set_seed, train_epoch
from .graph import KnowledgeGraph
from .models import build_model


def add_model_options(parser):
    parser.add_argument("--model", choices=("nbfnet", "astarnet"), default="nbfnet")
    parser.add_argument("--dim", type=int, default=32)
    parser.add_argument("--num-layers", type=int, default=6)
    parser.add_argument("--message", choices=("distmult", "transe", "rotate"),
                        default="distmult")
    parser.add_argument("--aggregate", choices=("pna", "sum", "mean", "max"),
                        default="pna")
    parser.add_argument("--short-cut", action=argparse.BooleanOptionalAction,
                        default=True)
    parser.add_argument("--layer-norm", action=argparse.BooleanOptionalAction,
                        default=True)
    parser.add_argument("--dependent", action=argparse.BooleanOptionalAction,
                        default=True)
    parser.add_argument("--node-ratio", type=float, default=0.1)
    parser.add_argument("--degree-ratio", type=float, default=1.0)


def model_config(args, graph):
    config = {
        "num_relations": graph.num_base_relations, "dim": args.dim,
        "num_layers": args.num_layers, "message": args.message,
        "aggregate": args.aggregate, "short_cut": args.short_cut,
        "layer_norm": args.layer_norm, "dependent": args.dependent,
    }
    if args.model == "astarnet":
        config.update(node_ratio=args.node_ratio, degree_ratio=args.degree_ratio)
    return config


def checkpoint_payload(args, graph, config, model, device, epoch=0):
    return {
        "model_name": args.model, "model_config": config,
        "state_dict": model.state_dict(), "entity_to_id": graph.entity_to_id,
        "relation_to_id": graph.relation_to_id, "data": str(Path(args.data).resolve()),
        "device_at_training": str(device), "epoch": epoch,
    }


def train(args):
    set_seed(args.seed)
    device = resolve_device(args.device)
    graph = KnowledgeGraph.from_directory(args.data)
    tensors = graph.graph(device)
    config = model_config(args, graph)
    model = build_model(args.model, config).to(device)
    optimizer = torch.optim.Adam(model.parameters(), lr=args.lr)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    known_train = graph.known_targets(training_only=True)
    known_all = graph.known_targets()
    print(json.dumps({"data": args.data, "nodes": graph.num_nodes,
                      "relations": graph.num_base_relations,
                      "observed_edges": len(graph.observed) if graph.observed is not None else len(graph.train),
                      "train_labels": graph.num_train_examples,
                      "signed_training": graph.signed_train is not None,
                      "model": args.model, "device": str(device)}), flush=True)
    best_mrr = -float("inf")
    best_epoch = 0
    train_started = time.monotonic()
    with (out / "history.jsonl").open("w") as history:
        for epoch in range(1, args.epochs + 1):
            loss, samples = train_epoch(
                model, tensors, graph.train, optimizer, known_train,
                args.batch_size, args.num_negative, args.strict_negative,
                args.adversarial_temperature, args.remove_one_hop,
                args.limit_train, signed_train=graph.signed_train)
            record = {"epoch": epoch, "train_loss": loss,
                      "train_samples": samples,
                      "elapsed_sec": time.monotonic() - train_started}
            if len(graph.valid) > 0 and \
                    (epoch % args.eval_every == 0 or epoch == args.epochs):
                validation = evaluate(model, tensors, graph.valid, known_all,
                                      args.eval_batch_size, args.limit_eval)
                record["valid"] = validation
                if validation["mrr"] > best_mrr:
                    best_mrr = validation["mrr"]
                    best_epoch = epoch
                    torch.save(checkpoint_payload(args, graph, config, model,
                                                  device, epoch), out / "best.pt")
            history.write(json.dumps(record) + "\n")
            history.flush()
            torch.save(checkpoint_payload(args, graph, config, model, device, epoch),
                       out / "last.pt")
            print(json.dumps(record), flush=True)
    train_wall = time.monotonic() - train_started
    checkpoint = out / ("best.pt" if best_epoch else "last.pt")
    saved = torch.load(checkpoint, map_location=device, weights_only=False)
    model.load_state_dict(saved["state_dict"])
    test = evaluate(model, tensors, graph.test, known_all,
                    args.eval_batch_size, args.limit_eval)
    result = {"model": args.model, "device": str(device),
              "train_wall_sec": train_wall, "best_epoch": best_epoch,
              "best_valid_mrr": best_mrr if best_epoch else None,
              "test": test, "model_config": config,
              "train_samples": graph.num_train_examples,
              "signed_training": graph.signed_train is not None,
              "eval_limit": args.limit_eval}
    (out / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2), flush=True)


def evaluate_checkpoint(args):
    device = resolve_device(args.device)
    saved = torch.load(args.checkpoint, map_location=device, weights_only=False)
    data = args.data or saved["data"]
    graph = KnowledgeGraph.from_directory(data)
    if graph.entity_to_id != saved["entity_to_id"] or \
            graph.relation_to_id != saved["relation_to_id"]:
        raise RuntimeError("checkpoint vocabulary does not match the input data")
    model = build_model(saved["model_name"], saved["model_config"])
    model.load_state_dict(saved["state_dict"])
    model = model.to(device)
    evaluation = graph.split(args.split)
    if len(evaluation) == 0:
        raise RuntimeError(f"evaluation split is empty: {args.split}")
    result = evaluate(model, graph.graph(device), evaluation,
                      graph.known_targets(), args.eval_batch_size, args.limit_eval)
    result.update(model=saved["model_name"], checkpoint=str(args.checkpoint),
                  split=args.split, device=str(device))
    if args.summary:
        Path(args.summary).write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    train_parser = subparsers.add_parser("train")
    train_parser.add_argument("--data", required=True)
    train_parser.add_argument("--out", required=True)
    add_model_options(train_parser)
    train_parser.add_argument("--device", default="auto")
    train_parser.add_argument("--epochs", type=int, default=20)
    train_parser.add_argument("--batch-size", type=int, default=4)
    train_parser.add_argument("--eval-batch-size", type=int, default=4)
    train_parser.add_argument("--num-negative", type=int, default=32)
    train_parser.add_argument("--lr", type=float, default=5e-3)
    train_parser.add_argument("--adversarial-temperature", type=float, default=0.5)
    train_parser.add_argument("--strict-negative", action=argparse.BooleanOptionalAction,
                              default=True)
    train_parser.add_argument("--remove-one-hop", action=argparse.BooleanOptionalAction,
                              default=True)
    train_parser.add_argument("--eval-every", type=int, default=1)
    train_parser.add_argument("--limit-train", type=int)
    train_parser.add_argument("--limit-eval", type=int)
    train_parser.add_argument("--seed", type=int, default=42)
    train_parser.set_defaults(func=train)

    eval_parser = subparsers.add_parser("eval")
    eval_parser.add_argument("--checkpoint", required=True)
    eval_parser.add_argument("--data")
    eval_parser.add_argument("--split", choices=("train", "valid", "test"),
                             default="test")
    eval_parser.add_argument("--device", default="auto")
    eval_parser.add_argument("--eval-batch-size", type=int, default=4)
    eval_parser.add_argument("--limit-eval", type=int)
    eval_parser.add_argument("--summary")
    eval_parser.set_defaults(func=evaluate_checkpoint)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
