"""Native PyTorch relational layers for generalized Bellman-Ford networks."""

from typing import Optional

import torch
from torch import nn
from torch.nn import functional as F

from .graph import GraphTensors


class GeneralizedRelationalConv(nn.Module):
    def __init__(self, dim: int, num_relations: int, message: str = "distmult",
                 aggregate: str = "pna", layer_norm: bool = True,
                 dependent: bool = True):
        super().__init__()
        if message not in ("distmult", "transe", "rotate"):
            raise ValueError(f"unsupported message function: {message}")
        if aggregate not in ("pna", "sum", "mean", "max"):
            raise ValueError(f"unsupported aggregate function: {aggregate}")
        if message == "rotate" and dim % 2:
            raise ValueError("rotate requires an even dimension")
        self.dim = dim
        self.num_relations = num_relations
        self.message_func = message
        self.aggregate_func = aggregate
        self.dependent = dependent
        self.relation_linear = (nn.Linear(dim, num_relations * dim)
                                if dependent else None)
        self.relation = (None if dependent else nn.Embedding(num_relations, dim))
        input_features = 13 * dim if aggregate == "pna" else 2 * dim
        self.linear = nn.Linear(input_features, dim)
        self.layer_norm = nn.LayerNorm(dim) if layer_norm else None

    def relation_input(self, query: torch.Tensor) -> torch.Tensor:
        if self.dependent:
            return self.relation_linear(query).view(
                len(query), self.num_relations, self.dim)
        return self.relation.weight.unsqueeze(0).expand(len(query), -1, -1)

    def compute_message(self, node_input: torch.Tensor,
                        edge_input: torch.Tensor) -> torch.Tensor:
        if self.message_func == "distmult":
            return node_input * edge_input
        if self.message_func == "transe":
            return node_input + edge_input
        node_re, node_im = node_input.chunk(2, dim=-1)
        edge_re, edge_im = edge_input.chunk(2, dim=-1)
        return torch.cat([
            node_re * edge_re - node_im * edge_im,
            node_re * edge_im + node_im * edge_re,
        ], dim=-1)

    def _aggregate(self, messages: torch.Tensor, groups: torch.Tensor,
                   hidden: torch.Tensor, boundary: torch.Tensor,
                   degree: torch.Tensor) -> torch.Tensor:
        batch_size, num_nodes, dim = hidden.shape
        size = batch_size * num_nodes
        flat_boundary = boundary.reshape(size, dim)
        index = groups.unsqueeze(-1).expand(-1, dim)
        sum_update = torch.zeros(size, dim, dtype=messages.dtype,
                                 device=messages.device)
        sum_update.index_add_(0, groups, messages)
        sum_update = sum_update + flat_boundary
        degree = degree.repeat(batch_size).reshape(size, 1).to(messages.dtype)
        mean = sum_update / degree
        if self.aggregate_func == "sum":
            update = sum_update
        elif self.aggregate_func == "mean":
            update = mean
        else:
            max_update = torch.full_like(sum_update, -torch.inf)
            min_update = torch.full_like(sum_update, torch.inf)
            max_update.scatter_reduce_(0, index, messages, reduce="amax",
                                       include_self=True)
            min_update.scatter_reduce_(0, index, messages, reduce="amin",
                                       include_self=True)
            max_update = torch.maximum(max_update, flat_boundary)
            min_update = torch.minimum(min_update, flat_boundary)
            if self.aggregate_func == "max":
                update = max_update
            else:
                square_sum = torch.zeros_like(sum_update)
                square_sum.index_add_(0, groups, messages.square())
                square_mean = (square_sum + flat_boundary.square()) / degree
                std = (square_mean - mean.square()).clamp(min=1e-6).sqrt()
                features = torch.stack([mean, max_update, min_update, std], dim=-1)
                log_degree = degree.log()
                scale = log_degree / log_degree.mean().clamp(min=1e-6)
                scales = torch.stack([
                    torch.ones_like(scale), scale,
                    scale.clamp(min=1e-2).reciprocal(),
                ], dim=-1)
                update = (features.flatten(-2).unsqueeze(-1) *
                          scales).flatten(-2)
        combined = self.linear(torch.cat([hidden.reshape(size, dim), update], dim=-1))
        if self.layer_norm is not None:
            combined = self.layer_norm(combined)
        return F.relu(combined).view(batch_size, num_nodes, dim)

    def forward(self, hidden: torch.Tensor, boundary: torch.Tensor,
                query: torch.Tensor, graph: GraphTensors,
                edge_index: Optional[torch.Tensor] = None,
                query_index: Optional[torch.Tensor] = None,
                source_priority: Optional[torch.Tensor] = None) -> torch.Tensor:
        batch_size, num_nodes, _ = hidden.shape
        relation_input = self.relation_input(query)
        if edge_index is None:
            edge_index = torch.arange(graph.num_edges, device=graph.device)
        if query_index is None:
            source = graph.head[edge_index]
            target = graph.tail[edge_index]
            relation = graph.relation[edge_index]
            node_input = hidden[:, source]
            edge_input = relation_input[:, relation]
            messages = self.compute_message(node_input, edge_input)
            if source_priority is not None:
                weight = torch.sigmoid(source_priority[:, source]).unsqueeze(-1)
                messages = messages * weight
            messages = messages.flatten(0, 1)
            groups = (torch.arange(batch_size, device=graph.device).unsqueeze(-1) *
                      num_nodes + target.unsqueeze(0)).flatten()
        else:
            source = graph.head[edge_index]
            target = graph.tail[edge_index]
            relation = graph.relation[edge_index]
            node_input = hidden[query_index, source]
            edge_input = relation_input[query_index, relation]
            messages = self.compute_message(node_input, edge_input)
            if source_priority is not None:
                weight = torch.sigmoid(source_priority[query_index, source]).unsqueeze(-1)
                messages = messages * weight
            groups = query_index * num_nodes + target
        return self._aggregate(messages, groups, hidden, boundary,
                               graph.in_degree + 1)
