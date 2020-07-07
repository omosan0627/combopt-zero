from graphviz import Graph
g = Graph(format='png')
N, M = map(int, input().split())
for i in range(N):
    g.node(str(i + 1))
for _ in range(M):
    a, b = map(int, input().split())
    g.edge(str(a), str(b))
g.view()
