"""Training and filtered-ranking evaluation for path-based KG models."""

import random
import time
from typing import Dict, Optional, Tuple

import torch
from torch.nn import functional as F

from .graph import GraphTensors, KnowledgeGraph
from .models import NBFNet


def set_seed(seed: int) -> None:
    random.seed(seed)
    torch.manual_seed(seed)
    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(seed)


def resolve_device(requested: str) -> torch.device:
    if requested != "auto":
        if requested.startswith("cuda") and not torch.cuda.is_available():
            raise RuntimeError("CUDA was requested but is unavailable")
        return torch.device(requested)
    if not torch.cuda.is_available():
        return torch.device("cpu")
    return torch.device(max(range(torch.cuda.device_count()),
                            key=lambda i: torch.cuda.mem_get_info(i)[0]))


def query_direction(triples: torch.Tensor, num_relations: int,
                    head_prediction: bool) -> Tuple[torch.Tensor, ...]:
    head, relation, tail = triples.unbind(-1)
    if head_prediction:
        return tail, relation + num_relations, head
    return head, relation, tail


def sample_negatives(sources: torch.Tensor, relations: torch.Tensor,
                     num_nodes: int, num_negative: int,
                     known: Dict[Tuple[int, int], set],
                     strict: bool) -> torch.Tensor:
    negatives = torch.randint(num_nodes, (len(sources), num_negative),
                              device=sources.device)
    if not strict:
        return negatives
    for i, (source, relation) in enumerate(zip(sources.tolist(), relations.tolist())):
        forbidden = known.get((source, relation), set())
        for j in range(num_negative):
            while int(negatives[i, j]) in forbidden:
                negatives[i, j] = torch.randint(num_nodes, (), device=sources.device)
    return negatives


def prediction_loss(model: NBFNet, graph: GraphTensors,
                    sources: torch.Tensor, relations: torch.Tensor,
                    targets: torch.Tensor, negatives: torch.Tensor,
                    adversarial_temperature: float) -> torch.Tensor:
    scores = model(graph, sources, relations)
    positive = scores.gather(1, targets.unsqueeze(-1)).squeeze(-1)
    negative_score = scores.gather(1, negatives)
    positive_loss = F.binary_cross_entropy_with_logits(
        positive, torch.ones_like(positive))
    negative_loss = F.binary_cross_entropy_with_logits(
        negative_score, torch.zeros_like(negative_score), reduction="none")
    if adversarial_temperature > 0:
        weight = F.softmax(adversarial_temperature * negative_score.detach(), dim=-1)
        negative_loss = (negative_loss * weight).sum(dim=-1)
    else:
        negative_loss = negative_loss.mean(dim=-1)
    return 0.5 * (positive_loss + negative_loss.mean())


def train_epoch(model: NBFNet, graph: GraphTensors, train: torch.Tensor,
                optimizer: torch.optim.Optimizer, known: Dict[Tuple[int, int], set],
                batch_size: int, num_negative: int, strict_negative: bool,
                adversarial_temperature: float, remove_one_hop: bool,
                limit: Optional[int] = None,
                signed_train: Optional[torch.Tensor] = None) -> Tuple[float, int]:
    model.train()
    signed = signed_train is not None
    examples = signed_train if signed else train
    if limit is not None:
        examples = examples[:limit]
    order = torch.randperm(len(examples))
    total_loss = 0.0
    total_samples = 0
    for start in range(0, len(examples), batch_size):
        triples = examples[order[start:start + batch_size]].to(graph.device)
        labels = triples[:, 3].float() if signed else None
        head, relation, tail = triples[:, :3].unbind(-1)
        head_prediction = (torch.arange(len(triples), device=graph.device) >=
                           len(triples) // 2)
        sources = torch.where(head_prediction, tail, head)
        relations = torch.where(head_prediction,
                                relation + graph.num_relations // 2, relation)
        targets = torch.where(head_prediction, head, tail)
        negatives = None
        if not signed:
            negatives = sample_negatives(sources, relations, graph.num_nodes,
                                         num_negative, known, strict_negative)
        batch_graph = graph
        if remove_one_hop:
            query_pairs = torch.stack([
                sources, torch.zeros_like(sources), targets], dim=-1)
            if not signed:
                negative_queries = torch.stack([
                    sources.unsqueeze(-1).expand_as(negatives),
                    torch.zeros_like(negatives), negatives], dim=-1).flatten(0, 1)
                query_pairs = torch.cat([query_pairs, negative_queries])
            batch_graph = graph.mask_pairs(query_pairs, remove_one_hop=True)
        if signed:
            scores = model(batch_graph, sources, relations)
            selected = scores.gather(1, targets.unsqueeze(-1)).squeeze(-1)
            loss = F.binary_cross_entropy_with_logits(selected, labels)
        else:
            loss = prediction_loss(model, batch_graph, sources, relations, targets,
                                   negatives, adversarial_temperature)
        optimizer.zero_grad()
        loss.backward()
        optimizer.step()
        total_loss += float(loss.detach()) * len(triples)
        total_samples += len(triples)
    return total_loss / max(total_samples, 1), total_samples


@torch.no_grad()
def evaluate(model: NBFNet, graph: GraphTensors, triples: torch.Tensor,
             known: Dict[Tuple[int, int], set], batch_size: int,
             limit: Optional[int] = None) -> Dict[str, float]:
    model.eval()
    if limit is not None:
        triples = triples[:limit]
    ranks = []
    started = time.monotonic()
    for start in range(0, len(triples), batch_size):
        batch = triples[start:start + batch_size].to(graph.device)
        for head_prediction in (False, True):
            sources, relations, targets = query_direction(
                batch, graph.num_relations // 2, head_prediction)
            scores = model(graph, sources, relations)
            for i, (source, relation, target) in enumerate(zip(
                    sources.tolist(), relations.tolist(), targets.tolist())):
                target_score = scores[i, target].clone()
                filtered = known.get((source, relation), set())
                if filtered:
                    scores[i, list(filtered)] = -torch.inf
                scores[i, target] = -torch.inf
                rank = int((scores[i] >= target_score).sum()) + 1
                ranks.append(rank)
    ranking = torch.tensor(ranks, dtype=torch.float64)
    elapsed = time.monotonic() - started
    return {
        "mr": float(ranking.mean()),
        "mrr": float((1.0 / ranking).mean()),
        "hits@1": float((ranking <= 1).double().mean()),
        "hits@3": float((ranking <= 3).double().mean()),
        "hits@10": float((ranking <= 10).double().mean()),
        "queries": len(ranks),
        "wall_sec": elapsed,
    }
