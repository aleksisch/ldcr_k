# The L_DCR grammars

The three languages L_D, L_C and L_R over the PAG, with their C++ additions, as first written
down (before L_D′ and L_RN).

## Edge labels

Every PAG edge carries a pair $(a, b)$:

- $a$ is the memory part, read by $L_D$;
- $b$ is the context part, read by $L_C$ and $L_R$.

In $b$:

- $\varepsilon$ means no context;
- $\hat c_j$ / $\check c_j$ mean entering / leaving a function at call site $j$;
- $\boxed{\hat c_j}$ / $\boxed{\check c_j}$ mean opening / closing the receiver lookup of the
  virtual call at site $j$.

Every edge also exists reversed. The reversed edge has $a$ barred and $b$ swapped
($\hat c \leftrightarrow \check c$).

| group | terminal | reversed |
|---|---|---|
| memory | $(\mathsf{new}_T, \varepsilon)$ | $(\overline{\mathsf{new}_T}, \varepsilon)$ |
| | $(\mathsf{assign}, \varepsilon)$ | $(\overline{\mathsf{assign}}, \varepsilon)$ |
| | $(\mathsf{store}[f], \varepsilon)$ | $(\overline{\mathsf{store}[f]}, \varepsilon)$ |
| | $(\mathsf{load}[f], \varepsilon)$ | $(\overline{\mathsf{load}[f]}, \varepsilon)$ |
| | $(\mathsf{load}[i], \varepsilon)$ | $(\overline{\mathsf{load}[i]}, \varepsilon)$ |
| | $(\mathsf{store}[0], \varepsilon)$ | $(\overline{\mathsf{store}[0]}, \varepsilon)$ |
| calls | $(\mathsf{dispatch}_T, \hat c_j)$ | $(\overline{\mathsf{dispatch}_T}, \check c_j)$ |
| | $(\mathsf{assign}, \hat c_j)$ | $(\overline{\mathsf{assign}}, \check c_j)$ |
| | $(\mathsf{assign}, \check c_j)$ | $(\overline{\mathsf{assign}}, \hat c_j)$ |
| dispatch | $(\mathsf{store}[i], \boxed{\hat c_j})$ | $(\overline{\mathsf{store}[i]}, \boxed{\check c_j})$ |
| | $(\mathsf{load}[0], \boxed{\check c_j})$ | $(\overline{\mathsf{load}[0]}, \boxed{\hat c_j})$ |
| | $(\mathsf{assign}, \boxed{\check c_j})$ | $(\overline{\mathsf{assign}}, \boxed{\hat c_j})$ |

- **Memory:** ordinary memory operations (allocate, copy, store, load).
- **Calls:** calls of non-virtual functions.
- **Dispatch:** a virtual call opens ($\boxed{\hat c_j}$) and closes ($\boxed{\check c_j}$) at
  its receiver.

## PAG construction

| instruction | edges |
|---|---|
| $x = \mathbf{new}\ T$ (object $O$) | $O \xrightarrow{(\mathsf{new}_T,\ \varepsilon)} x$ |
| $x = y$ | $y \xrightarrow{(\mathsf{assign},\ \varepsilon)} x$ |
| $x = y.f$ | $y \xrightarrow{(\mathsf{load}[f],\ \varepsilon)} x$ |
| $x.f = y$ | $y \xrightarrow{(\mathsf{store}[f],\ \varepsilon)} x$ |
| $x = m(a_1, \ldots, a_n)$ at site $c_j$ | $a_i \xrightarrow{(\mathsf{assign},\ \hat c_j)} p^m_i$ for all $i$ |
| | $\mathit{ret}^m \xrightarrow{(\mathsf{assign},\ \check c_j)} x$ |
| non-static method $X::M(a_1, \ldots, a_n)$ | $\mathit{this}^M \xrightarrow{(\mathsf{load}[i],\ \varepsilon)} p^M_i$ for all $i$ |
| | $\mathit{ret}^M \xrightarrow{(\mathsf{store}[0],\ \varepsilon)} \mathit{this}^M$ |
| $x = r.m(a_1, \ldots, a_n)$ at site $c_j$ | $a_i \xrightarrow{(\mathsf{store}[i],\ \boxed{\hat c_j})} r$ for all $i$ |
| | $r \xrightarrow{(\mathsf{load}[0],\ \boxed{\check c_j})} x$ |
| | $r \xrightarrow{(\mathsf{assign},\ \varepsilon)} r\#c_j$ |
| | $r \xrightarrow{(\mathsf{assign},\ \boxed{\check c_j})} r\#c_j$ |
| | $r\#c_j \xrightarrow{(\mathsf{dispatch}_t,\ \hat c_j)} \mathit{this}^{m'}$ for all $t \in \mathrm{DeclTypeOf}(r)$, $m' = \mathrm{dispatch}(c_j, t)$ |

A virtual call passes its arguments through fields of the receiver: argument $i$ is stored
into field $i$ of $r$ and read back by the callee as $\mathsf{load}[i]$ of its $\mathit{this}$.
The result travels the same way through field $0$.

## Shorthands

$$
\begin{aligned}
\mathit{Store}_f &\to (\mathsf{store}[f], \varepsilon) &
\mathit{Store}_i &\to (\mathsf{store}[i], \boxed{\hat c_j}) &
\mathit{Store}_0 &\to (\mathsf{store}[0], \varepsilon) \\
\mathit{Load}_f &\to (\mathsf{load}[f], \varepsilon) &
\mathit{Load}_i &\to (\mathsf{load}[i], \varepsilon) &
\mathit{Load}_0 &\to (\mathsf{load}[0], \boxed{\check c_j}) \\
\overline{\mathit{Store}}_f &\to (\overline{\mathsf{store}[f]}, \varepsilon) &
\overline{\mathit{Store}}_i &\to (\overline{\mathsf{store}[i]}, \boxed{\check c_j}) &
\overline{\mathit{Store}}_0 &\to (\overline{\mathsf{store}[0]}, \varepsilon) \\
\overline{\mathit{Load}}_f &\to (\overline{\mathsf{load}[f]}, \varepsilon) &
\overline{\mathit{Load}}_i &\to (\overline{\mathsf{load}[i]}, \varepsilon) &
\overline{\mathit{Load}}_0 &\to (\overline{\mathsf{load}[0]}, \boxed{\hat c_j})
\end{aligned}
$$

$$
\begin{aligned}
\mathit{Asgn} &\to (\mathsf{assign}, \varepsilon) \mid (\mathsf{assign}, \hat c_j)
  \mid (\mathsf{assign}, \check c_j) \mid (\mathsf{assign}, \boxed{\check c_j}) \\
\overline{\mathit{Asgn}} &\to (\overline{\mathsf{assign}}, \varepsilon)
  \mid (\overline{\mathsf{assign}}, \check c_j) \mid (\overline{\mathsf{assign}}, \hat c_j)
  \mid (\overline{\mathsf{assign}}, \boxed{\hat c_j})
\end{aligned}
$$

$(\ast, b)$ is any edge whose context part is $b$:

$$
\begin{aligned}
(\ast, \hat c_j) &\to (\mathsf{dispatch}_T, \hat c_j) \mid (\mathsf{assign}, \hat c_j)
  \mid (\overline{\mathsf{assign}}, \hat c_j) \\
(\ast, \check c_j) &\to (\overline{\mathsf{dispatch}_T}, \check c_j) \mid (\mathsf{assign}, \check c_j)
  \mid (\overline{\mathsf{assign}}, \check c_j) \\
(\ast, \boxed{\hat c_j}) &\to \mathit{Store}_i \mid \overline{\mathit{Load}}_0
  \mid (\overline{\mathsf{assign}}, \boxed{\hat c_j}) \\
(\ast, \boxed{\check c_j}) &\to \mathit{Load}_0 \mid \overline{\mathit{Store}}_i
  \mid (\mathsf{assign}, \boxed{\check c_j}) \\
(\ast, \varepsilon) &\to (\mathsf{new}_T, \varepsilon) \mid (\mathsf{assign}, \varepsilon)
  \mid \mathit{Store}_f \mid \mathit{Load}_f \mid \mathit{Load}_i \mid \mathit{Store}_0 \\
  &\mid (\overline{\mathsf{new}_T}, \varepsilon) \mid (\overline{\mathsf{assign}}, \varepsilon)
  \mid \overline{\mathit{Store}}_f \mid \overline{\mathit{Load}}_f \mid \overline{\mathit{Load}}_i
  \mid \overline{\mathit{Store}}_0
\end{aligned}
$$

## $L_D$: fields and dispatch

Reads the memory part $a$. A path from object $O$ to variable $x$ means $x$ may point to $O$.

$$
\begin{aligned}
S &\to \mathit{Flowsto}_T \\
\mathit{Flowsto}_T &\to (\mathsf{new}_T, \varepsilon)
  \mid \mathit{Flowsto}_T\ (\mathsf{dispatch}_T, \hat c_j)
  \mid \mathit{Flowsto}_T\ \mathit{Flows} \\
\mathit{Flows} &\to \mathit{Asgn}
  \mid \mathit{Store}_f\ \mathit{Alias}\ \mathit{Load}_f
  \mid \mathit{Store}_i\ \mathit{Alias}\ \mathit{Load}_i
  \mid \mathit{Store}_0\ \mathit{Alias}\ \mathit{Load}_0 \\
\mathit{Alias} &\to \overline{\mathit{Flowsto}}_T\ \mathit{Flowsto}_T \\
\overline{\mathit{Flowsto}}_T &\to (\overline{\mathsf{new}_T}, \varepsilon)
  \mid (\overline{\mathsf{dispatch}_T}, \check c_j)\ \overline{\mathit{Flowsto}}_T
  \mid \overline{\mathit{Flows}}\ \overline{\mathit{Flowsto}}_T \\
\overline{\mathit{Flows}} &\to \overline{\mathit{Asgn}}
  \mid \overline{\mathit{Load}}_f\ \mathit{Alias}\ \overline{\mathit{Store}}_f
  \mid \overline{\mathit{Load}}_i\ \mathit{Alias}\ \overline{\mathit{Store}}_i
  \mid \overline{\mathit{Load}}_0\ \mathit{Alias}\ \overline{\mathit{Store}}_0
\end{aligned}
$$

$\mathsf{dispatch}_T$ admits only objects of type $T$: a virtual call reaches the method of
the receiver's dynamic type.

## $L_C$: calls and returns match

Reads the context part $b$. Calls and returns match per call site. Unmatched returns may come
first (the path starts inside a callee) and unmatched calls last (the path ends inside a
callee). Dispatch boxes are plain steps.

$$
\begin{aligned}
\mathit{Real} &\to \mathit{Exit}\ \mathit{Entry} \\
\mathit{Exit} &\to \mathit{Exit}\ \mathit{Balanced} \mid \mathit{Exit}\ (\ast, \check c_j)
  \mid \varepsilon \\
\mathit{Entry} &\to \mathit{Entry}\ \mathit{Balanced} \mid \mathit{Entry}\ (\ast, \hat c_j)
  \mid \varepsilon \\
\mathit{Balanced} &\to \mathit{Balanced}\ \mathit{Balanced}
  \mid (\ast, \hat c_j)\ \mathit{Balanced}\ (\ast, \check c_j)
  \mid \mathit{Skip} \mid \varepsilon \\
\mathit{Skip} &\to (\ast, \varepsilon) \mid (\ast, \boxed{\hat c_j}) \mid (\ast, \boxed{\check c_j})
\end{aligned}
$$

## $L_R$: paths inside a dispatch

Reads the context part $b$. A receiver lookup opened at site $j$ closes at site $j$. Every call
it leaves inside the box is re-entered through the same site (the mirrored
$\check c_j \ldots \hat c_j$ of $\mathit{CtxRec}$).

$$
\begin{aligned}
\mathit{RecCtx} &\to \mathit{RecCtx}\ (\ast, \hat c_j) \mid \mathit{RecCtx}\ (\ast, \check c_j)
  \mid \mathit{RecCtx}\ (\ast, \varepsilon) \mid \mathit{RecCtx}\ \mathit{SiteRec}
  \mid \varepsilon \\
\mathit{SiteRec} &\to (\ast, \boxed{\hat c_j})\ \mathit{CtxRec}\ (\ast, \boxed{\check c_j}) \\
\mathit{CtxRec} &\to \mathit{Matched}\ \mathit{CtxRec} \mid \mathit{CtxRec}\ \mathit{Matched}
  \mid (\ast, \check c_j)\ \mathit{CtxRec}\ (\ast, \hat c_j) \mid \varepsilon \\
\mathit{Matched} &\to \mathit{Matched}\ \mathit{Matched}
  \mid (\ast, \hat c_j)\ \mathit{Matched}\ (\ast, \check c_j)
  \mid \mathit{SiteRec} \mid (\ast, \varepsilon) \mid \varepsilon
\end{aligned}
$$

## C++ additions

### PAG

Types get a bottom element $\bot$: no type known.

$\mathrm{Members}(T)$ lists the members of class $T$:

- $(k, M)$: member $k$ of class $M$, held by value;
- $(k, \bot)$: a field whose address is taken.

$[\mathrm{Member}](O, x, T)$: for every $(k, M) \in \mathrm{Members}(T)$, add

$$
O.k \xrightarrow{(\mathsf{new}_M,\ \varepsilon)} t, \qquad
t \xrightarrow{(\mathsf{store}[T::k],\ \varepsilon)} x, \qquad
[\mathrm{Member}](O.k, t, M).
$$

So creating an object also creates a $\mathsf{new}$ edge for each of its members that has no
constructor of its own.

| instruction | edges |
|---|---|
| `x = alloca T` (object $O$) | $O \xrightarrow{(\mathsf{new}_T,\ \varepsilon)} x$, $[\mathrm{Member}](O, x, T)$ |
| `x = alloca` (object $O$) | $O \xrightarrow{(\mathsf{new}_\bot,\ \varepsilon)} x$ |
| `x = call new` (object $O$) | $O \xrightarrow{(\mathsf{new}_\bot,\ \varepsilon)} x$ |
| `call @T::T(q, a_1, …, a_n)` at site $c_j$ | as an ordinary call, plus $q \xrightarrow{(\mathsf{ctor}_T,\ \varepsilon)} q$ and $[\mathrm{Member}](O_j, q, T)$ |
| `call memcpy(a, b)` of type $S$ | for every pointer $f$ at field path $\pi$ in $S$: $b \xrightarrow{\mathsf{load}[\pi]\ \mathsf{load}[f]} t \xrightarrow{\mathsf{store}[f]} a'$ and $a \xrightarrow{\mathsf{load}[\pi]} a'$ |
| `call memcpy(a, b, n)` | ignored, like all pointer arithmetic |

### $L_D$

An untyped object gets the type of the constructor that runs on it:

$$
\mathit{Flowsto}_T \to \ldots \mid \mathit{Flowsto}_\bot\ (\mathsf{ctor}_T, \varepsilon),
\qquad
\overline{\mathit{Flowsto}}_T \to \ldots \mid (\overline{\mathsf{ctor}_T}, \varepsilon)\
\overline{\mathit{Flowsto}}_\bot
$$

## Intersection

A pair $(O, x)$ is a points-to fact of $L_{DCR}$ when one path from $O$ to $x$ is in all three
languages. That is undecidable in general (two interleaved stacks), so the tool intersects the
languages at the endpoints, or by mutual refinement (`--refine`).
