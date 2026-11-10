"""Knowledge-graph TSV input and inverse-augmented tensor graphs."""

from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple

import torch


@dataclass(frozen=True)
class GraphTensors:
    head: torch.Tensor
    tail: torch.Tensor
    relation: torch.Tensor
    out_offsets: torch.Tensor
    out_degree: torch.Tensor
    in_degree: torch.Tensor
    num_nodes: int
    num_relations: int

    @property
    def num_edges(self) -> int:
        return self.head.numel()

    @property
    def device(self) -> torch.device:
        return self.head.device

    @classmethod
    def from_edges(cls, head: torch.Tensor, tail: torch.Tensor,
                   relation: torch.Tensor, num_nodes: int,
                   num_relations: int) -> "GraphTensors":
        order = torch.argsort(head, stable=True)
        head = head[order].contiguous()
        tail = tail[order].contiguous()
        relation = relation[order].contiguous()
        out_degree = torch.bincount(head, minlength=num_nodes)
        out_offsets = torch.zeros(num_nodes + 1, dtype=torch.long,
                                  device=head.device)
        out_offsets[1:] = torch.cumsum(out_degree, dim=0)
        in_degree = torch.bincount(tail, minlength=num_nodes)
        return cls(head, tail, relation, out_offsets, out_degree, in_degree,
                   num_nodes, num_relations)

    @classmethod
    def from_triples(cls, triples: torch.Tensor, num_nodes: int,
                     num_base_relations: int,
                     device: torch.device) -> "GraphTensors":
        triples = triples.to(device)
        head, relation, tail = triples.unbind(-1)
        return cls.from_edges(
            torch.cat([head, tail]), torch.cat([tail, head]),
            torch.cat([relation, relation + num_base_relations]), num_nodes,
            2 * num_base_relations)

    def mask_pairs(self, triples: torch.Tensor,
                   remove_one_hop: bool = True) -> "GraphTensors":
        triples = triples.to(self.device)
        head, relation, tail = triples.unbind(-1)
        n = self.num_nodes
        if remove_one_hop:
            edge_key = self.head * n + self.tail
            positive_key = torch.cat([head * n + tail, tail * n + head])
        else:
            r = self.num_relations
            edge_key = (self.head * r + self.relation) * n + self.tail
            positive_key = torch.cat([
                (head * r + relation) * n + tail,
                (tail * r + relation + r // 2) * n + head,
            ])
        keep = ~torch.isin(edge_key, positive_key)
        return self.from_edges(self.head[keep], self.tail[keep],
                               self.relation[keep], n, self.num_relations)


class KnowledgeGraph:
    """KG labels with optional observed-graph and signed-training separation."""

    def __init__(self, train: torch.Tensor, valid: torch.Tensor,
                 test: torch.Tensor, entity_to_id: Dict[str, int],
                 relation_to_id: Dict[str, int], observed: torch.Tensor = None,
                 signed_train: torch.Tensor = None):
        self.train = train.long()
        self.valid = valid.long()
        self.test = test.long()
        self.observed = None if observed is None else observed.long()
        self.signed_train = None if signed_train is None else signed_train.long()
        self.entity_to_id = entity_to_id
        self.relation_to_id = relation_to_id
        self.num_nodes = len(entity_to_id)
        self.num_base_relations = len(relation_to_id)
        self._known = {}

    @property
    def num_train_examples(self) -> int:
        return (len(self.train) if self.signed_train is None else
                len(self.signed_train))

    @classmethod
    def from_directory(cls, path: str) -> "KnowledgeGraph":
        root = Path(path)
        entity_to_id: Dict[str, int] = {}
        relation_to_id: Dict[str, int] = {}
        names = (("observed",) if (root / "observed.txt").exists() else ()) + \
                ("train", "valid", "test")
        tensors = {}
        for name in names:
            file_path = root / f"{name}.txt"
            if name == "valid" and not file_path.exists():
                tensors[name] = torch.empty(0, 3, dtype=torch.long)
                continue
            rows: List[Tuple[int, int, int]] = []
            signed_rows: List[Tuple[int, int, int, int]] = []
            labels = []
            with file_path.open() as stream:
                for line_no, line in enumerate(stream, 1):
                    fields = line.rstrip("\n").split("\t")
                    if len(fields) not in (3, 4) or (len(fields) == 4 and name != "train"):
                        raise ValueError(f"{name}.txt:{line_no}: expected h\\tr\\tt")
                    h, r, t = fields[:3]
                    h_id = entity_to_id.setdefault(h, len(entity_to_id))
                    r_id = relation_to_id.setdefault(r, len(relation_to_id))
                    t_id = entity_to_id.setdefault(t, len(entity_to_id))
                    if len(fields) == 4:
                        if fields[3] not in ("0", "1"):
                            raise ValueError(f"{name}.txt:{line_no}: label must be 0/1")
                        label = int(fields[3])
                        labels.append(label)
                        signed_rows.append((h_id, r_id, t_id, label))
                        if label:
                            rows.append((h_id, r_id, t_id))
                    else:
                        rows.append((h_id, r_id, t_id))
            if labels and len(labels) != len(signed_rows):
                raise AssertionError("signed-row accounting error")
            if labels and len(rows) + sum(label == 0 for label in labels) != len(labels):
                raise AssertionError("mixed signed/unsigned training rows")
            tensors[name] = torch.tensor(rows, dtype=torch.long).reshape(-1, 3)
            if signed_rows:
                tensors["signed_train"] = torch.tensor(
                    signed_rows, dtype=torch.long).reshape(-1, 4)
        return cls(tensors["train"], tensors["valid"], tensors["test"],
                   entity_to_id, relation_to_id, tensors.get("observed"),
                   tensors.get("signed_train"))

    def split(self, name: str) -> torch.Tensor:
        if name not in ("train", "valid", "test"):
            raise ValueError(f"unknown split: {name}")
        return getattr(self, name)

    def graph(self, device: torch.device, split: str = "train") -> GraphTensors:
        if self.observed is not None:
            if split != "train":
                raise ValueError("observed-graph datasets use split='train' for the graph")
            triples = self.observed
        else:
            triples = self.split(split)
        return GraphTensors.from_triples(triples, self.num_nodes,
                                         self.num_base_relations, device)

    def known_targets(self, training_only: bool = False) -> Dict[Tuple[int, int], set]:
        key = "train" if training_only else "all"
        if key in self._known:
            return self._known[key]
        splits = (self.train,) if training_only else (self.train, self.valid, self.test)
        if self.observed is not None:
            splits = (self.observed, *splits)
        known: Dict[Tuple[int, int], set] = {}
        r = self.num_base_relations
        for triples in splits:
            for h, rel, t in triples.tolist():
                known.setdefault((h, rel), set()).add(t)
                known.setdefault((t, rel + r), set()).add(h)
        self._known[key] = known
        return known

    def relation_id(self, name: str) -> int:
        return self.relation_to_id[name]

    def entity_id(self, name: str) -> int:
        return self.entity_to_id[name]
