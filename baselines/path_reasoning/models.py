"""NBFNet and A*Net implemented with native PyTorch operations."""

from typing import Dict, List, Tuple

import torch
from torch import nn

from .graph import GraphTensors
from .layers import GeneralizedRelationalConv


def _mlp(input_dim: int, hidden_dim: int) -> nn.Sequential:
    return nn.Sequential(nn.Linear(input_dim, hidden_dim), nn.ReLU(),
                         nn.Linear(hidden_dim, 1))


class NBFNet(nn.Module):
    def __init__(self, num_relations: int, dim: int = 32,
                 num_layers: int = 6, message: str = "distmult",
                 aggregate: str = "pna", short_cut: bool = True,
                 layer_norm: bool = True, dependent: bool = True):
        super().__init__()
        self.num_base_relations = num_relations
        self.dim = dim
        self.num_layers = num_layers
        self.short_cut = short_cut
        self.model_config = {
            "num_relations": num_relations, "dim": dim,
            "num_layers": num_layers, "message": message,
            "aggregate": aggregate, "short_cut": short_cut,
            "layer_norm": layer_norm, "dependent": dependent,
        }
        self.query = nn.Embedding(2 * num_relations, dim)
        self.layers = nn.ModuleList([
            GeneralizedRelationalConv(dim, 2 * num_relations, message,
                                      aggregate, layer_norm, dependent)
            for _ in range(num_layers)
        ])
        self.mlp = _mlp(2 * dim, 2 * dim)
        self.last_num_messages: List[int] = []

    def forward(self, graph: GraphTensors, sources: torch.Tensor,
                relations: torch.Tensor) -> torch.Tensor:
        query = self.query(relations)
        batch_size = len(sources)
        boundary = torch.zeros(batch_size, graph.num_nodes, self.dim,
                               dtype=query.dtype, device=query.device)
        boundary[torch.arange(batch_size, device=query.device), sources] = query
        hidden = boundary
        self.last_num_messages = []
        for layer in self.layers:
            update = layer(hidden, boundary, query, graph)
            if self.short_cut:
                update = update + hidden
            hidden = update
            self.last_num_messages.append(graph.num_edges)
        node_query = query.unsqueeze(1).expand(-1, graph.num_nodes, -1)
        return self.mlp(torch.cat([hidden, node_query], dim=-1)).squeeze(-1)


class AStarNet(NBFNet):
    def __init__(self, num_relations: int, dim: int = 32,
                 num_layers: int = 6, message: str = "distmult",
                 aggregate: str = "pna", short_cut: bool = True,
                 layer_norm: bool = True, dependent: bool = True,
                 node_ratio: float = 0.1, degree_ratio: float = 1.0):
        super().__init__(num_relations, dim, num_layers, message, aggregate,
                         short_cut, layer_norm, dependent)
        if not 0.0 <= node_ratio <= 1.0 or degree_ratio < 0.0:
            raise ValueError("invalid A*Net node/degree ratio")
        self.node_ratio = node_ratio
        self.degree_ratio = degree_ratio
        self.model_config.update({
            "node_ratio": node_ratio, "degree_ratio": degree_ratio,
        })
        self.priority_linear = nn.Linear(2 * dim, dim)
        self.mlp = _mlp(dim, 2 * dim)
        self.last_num_nodes: List[int] = []

    def _priority(self, hidden: torch.Tensor,
                  query: torch.Tensor) -> torch.Tensor:
        heuristic = self.priority_linear(torch.cat([hidden, query], dim=-1))
        message = self.layers[0].compute_message(hidden, heuristic)
        return self.mlp(message).squeeze(-1)

    def _select_edges(self, graph: GraphTensors,
                      score: torch.Tensor) -> Tuple[torch.Tensor, torch.Tensor, int]:
        batch_size = len(score)
        k = min(graph.num_nodes, int(self.node_ratio * graph.num_nodes))
        if k == 0:
            empty = torch.empty(0, dtype=torch.long, device=graph.device)
            return empty, empty, 0
        selected = score.topk(k, dim=1).indices
        selected_queries = (torch.arange(batch_size, device=graph.device)
                            .unsqueeze(-1).expand(-1, k).flatten())
        flat_selected = selected.flatten()
        degrees = graph.out_degree[flat_selected]
        starts = graph.out_offsets[flat_selected]
        total = int(degrees.sum())
        if total == 0:
            empty = torch.empty(0, dtype=torch.long, device=graph.device)
            return empty, empty, k
        cumulative = torch.cumsum(degrees, dim=0) - degrees
        edge_index = (torch.arange(total, device=graph.device) -
                      torch.repeat_interleave(cumulative, degrees) +
                      torch.repeat_interleave(starts, degrees))
        query_index = torch.repeat_interleave(selected_queries, degrees)
        priority = torch.sigmoid(score[query_index, graph.tail[edge_index]])
        composite = priority + 2.0 * query_index.to(priority.dtype)
        order = torch.argsort(composite, descending=True, stable=True)
        sorted_queries = query_index[order]
        positions = torch.arange(total, device=graph.device)
        new_group = torch.ones(total, dtype=torch.bool, device=graph.device)
        new_group[1:] = sorted_queries[1:] != sorted_queries[:-1]
        group_start = torch.cummax(
            torch.where(new_group, positions, torch.zeros_like(positions)),
            dim=0).values
        rank = positions - group_start
        edge_limit = int(self.degree_ratio * k * graph.num_edges / graph.num_nodes)
        keep = rank < edge_limit
        selected_edges = order[keep]
        return query_index[selected_edges], edge_index[selected_edges], k

    def forward(self, graph: GraphTensors, sources: torch.Tensor,
                relations: torch.Tensor) -> torch.Tensor:
        query = self.query(relations)
        batch_size = len(sources)
        boundary = torch.zeros(batch_size, graph.num_nodes, self.dim,
                               dtype=query.dtype, device=query.device)
        batch_index = torch.arange(batch_size, device=query.device)
        boundary[batch_index, sources] = query
        hidden = boundary.clone()
        initial = self._priority(torch.zeros_like(query), query)
        score = initial.unsqueeze(-1).expand(-1, graph.num_nodes).clone()
        score[batch_index, sources] = self._priority(query, query)
        self.last_num_messages = []
        self.last_num_nodes = []
        for layer in self.layers:
            query_index, edge_index, k = self._select_edges(graph, score)
            self.last_num_nodes.append(k)
            if edge_index.numel() == 0:
                self.last_num_messages.append(0)
                continue
            update = layer(hidden, boundary, query, graph, edge_index,
                           query_index, source_priority=score)
            updated = torch.zeros(batch_size, graph.num_nodes,
                                  dtype=torch.bool, device=graph.device)
            updated[query_index, graph.tail[edge_index]] = True
            if self.short_cut:
                update = update + hidden
            hidden = torch.where(updated.unsqueeze(-1), update, hidden)
            locations = updated.nonzero(as_tuple=False)
            new_score = self._priority(
                hidden[locations[:, 0], locations[:, 1]], query[locations[:, 0]])
            score = score.reshape(-1).index_copy(
                0, locations[:, 0] * graph.num_nodes + locations[:, 1],
                new_score).view(batch_size, graph.num_nodes)
            self.last_num_messages.append(edge_index.numel())
        return score


def build_model(name: str, config: Dict) -> NBFNet:
    if name == "nbfnet":
        return NBFNet(**config)
    if name == "astarnet":
        return AStarNet(**config)
    raise ValueError(f"unknown model: {name}")
