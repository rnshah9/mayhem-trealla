:- module(lists, [
	member/2, select/3, selectchk/3, subtract/3, union/3,
	intersection/3, reverse/2, append/2, nth/3, nth1/3, nth0/3,
	last/2, flatten/2, append/3, same_length/2, sum_list/2,
	toconjunction/2, numlist/3, length/2
	]).

member(X, [X|_]).
member(X, [_|Xs]) :- member(X, Xs).

append([], L, L).
append([H|T], L, [H|R]) :-
	append(T, L, R).

select(X, [X|T], T).
select(X, [H|T], [H|Rest]) :- select(X, T, Rest).

selectchk(X, L, Rest) :- select(X, L, Rest), !.

subtract([], _, []) :- !.
subtract([H|T], L2, L3) :- memberchk(H, L2), !, subtract(T, L2, L3).
subtract([H|T1], L2, [H|T3]) :- subtract(T1, L2, T3).

union([], L, L).
union([H|T], Y, Z):- member(H, Y), !, union(T, Y, Z).
union([H|T], Y, [H|Z]):- union(T, Y, Z).

intersection([], _, []).
intersection([H|T], Y, [H|Z]) :- member(H, Y), !, intersection(T, Y, Z).
intersection([_|T], Y, Z) :- intersection(T, Y, Z).

reverse(L1, L2) :- revzap_(L1, [], L2).

revzap_([], L, L) :- !.
revzap_([H|L], L2, L3) :- revzap_(L, [H|L2], L3).

append(ListOfLists, List) :-
	must_be(ListOfLists, list, _, _),
	append_(ListOfLists, List).

append_([], []) :- !.
append_([L|Ls], As) :-
	append(L, Ws, As),
	append_(Ls, Ws).

nth(1, [H|_], H).
nth(N, [_|T], H) :- nth(M, T, H), N is M + 1.

nth1(1, [H|_], H).
nth1(N, [_|T], H) :- nth1(M, T, H), N is M + 1.

nth0(0, [H|_], H).
nth0(N, [_|T], H) :- nth0(M, T, H), N is M + 1.

last_([], Last, Last).
last_([X|Xs], _, Last) :- last_(Xs, X, Last).

last([X|Xs], Last) :- last_(Xs, X, Last).

flatten(List, FlatList) :-
    flatten_(List, [], FlatList0),
    !,
    FlatList = FlatList0.

flatten_(Var, Tl, [Var|Tl]) :-
    var(Var),
    !.
flatten_([], Tl, Tl) :- !.
flatten_([Hd|Tl], Tail, List) :-
    !,
    flatten_(Hd, FlatHeadTail, List),
    flatten_(Tl, Tail, FlatHeadTail).
flatten_(NonList, Tl, [NonList|Tl]).

same_length([], []).
same_length([_|As], [_|Bs]) :-
        same_length(As, Bs).

sum_list(Ls, S) :-
        foldl(lists:sum_, Ls, 0, S).

sum_(L, S0, S) :- S is S0 + L.

toconjunction(List0, Goal) :-
	reverse(List0, List),
	toconjunction_(List, true, Goal).

toconjunction_([], In, In).
toconjunction_([H|T], true, Out) :- !,
	Out2 = H,
	toconjunction_(T, Out2, Out).
toconjunction_([H|T], In, Out) :-
	Out2 = (H, In),
	toconjunction_(T, Out2, Out).

numlist(L, U, Ns) :-
	must_be(integer, L),
	must_be(integer, U),
	L =< U,
	numlist_(L, U, Ns).

numlist_(U, U, List) :-
	!,
	List = [U].
numlist_(L, U, [L|Ns]) :-
	L2 is L+1,
	numlist_(L2, U, Ns).

/*
  This implementation of length/2 differs from the ISO draft in that
  it type-checks the first argument.
*/

length(Xs0, N) :-
	'$skip_max_list'(M, N, Xs0, Xs),
	( Xs == [] -> N = M
	; var(Xs), Xs == N -> fail
	; Xs \= [_|_] -> throw(error(type_error(list,Xs0),length/2))
	; nonvar(N), '$skip_max_list'(_, Max, Xs, _), Max == -1 -> throw(error(type_error(list,Xs0),length/2))
	; nonvar(Xs) -> var(N), throw(error(resource_error(finite_memory),length/2))
	; nonvar(N) -> R is N-M, length_rundown(Xs, R)
	; N == Xs -> throw(error(resource_error(finite_memory),length/2))
	; length_addendum(Xs, N, M)
	).

length_addendum([], N, N).
length_addendum([_|Xs], N, M) :-
    M1 is M + 1,
    length_addendum(Xs, N, M1).

length_rundown(Xs, 0) :- !, Xs = [].
length_rundown([_|Xs], N) :-
    N1 is N-1,
    length_rundown(Xs, N1).
