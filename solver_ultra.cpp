// POLYLOG ULTRA: exact hybrid SAT solver, C++17, standard library only.
// Build: g++ -O3 -DNDEBUG -std=c++17 solver_ultra.cpp -o solver_ultra
// Run:   ./solver_ultra input.cnf --output result.txt
// Small configuration graphs + component decomposition + 2-SAT + CDCL.
// This extends the previous paper-based graph engine; see README_ULTRA.md.
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using Lit = int; // 2*variable + sign; positive sign=0, negative sign=1.
using Clause = std::vector<Lit>;
using CNF = std::vector<Clause>;
using U64 = std::uint64_t;
using Clock = std::chrono::steady_clock;
static int var(Lit p) { return p >> 1; }
static double elapsed(Clock::time_point t) { return std::chrono::duration<double>(Clock::now()-t).count(); }
struct TimeLimit {};
struct Budget {
    Clock::time_point start = Clock::now();
    double limit = 0;
    void check() const { if (limit > 0 && elapsed(start) >= limit) throw TimeLimit{}; }
};
struct Options {
    std::string input, output, stats_file, engine = "auto";
    bool json = false;
    double timeout = 0;
    int dp_width = 18;
    U64 frontier_budget = 4096, node_budget = 262144;
    U64 restart_base = 100, reduce_every = 2000;
};
struct Stats {
    U64 variables=0, clauses=0, residual_variables=0, residual_clauses=0;
    U64 root_assignments=0, components=0, solved_components=0;
    U64 two_sat_components=0, graph_components=0, graph_fallbacks=0;
    U64 graph_nodes=0, peak_frontier=0, original_width_max=0, ordered_width_max=0;
    U64 cdcl_components=0, decisions=0, conflicts=0, propagations=0;
    U64 learned_created=0, learned_deleted=0, reductions=0, garbage_collections=0, restarts=0;
    double parse_seconds=0, preprocess_seconds=0, search_seconds=0, verify_seconds=0, total_seconds=0;
};

class FastInput {
    FILE* file;
    char buffer[1<<16];
    std::size_t at=0, end=0;
public:
    explicit FastInput(const std::string& path) : file(std::fopen(path.c_str(),"rb")) {
        if (!file) throw std::runtime_error("cannot open input: "+path);
    }
    ~FastInput() { std::fclose(file); }
    int peek() {
        if (at==end) { end=std::fread(buffer,1,sizeof(buffer),file); at=0; if (!end) return EOF; }
        return static_cast<unsigned char>(buffer[at]);
    }
    int get() { int c=peek(); if (c!=EOF) ++at; return c; }
    int space() { int c; while ((c=peek())!=EOF && c<=32) get(); return c; }
    void line() { int c; while ((c=get())!=EOF && c!='\n') {} }
    std::string word() { space(); std::string s; while (peek()!=EOF && peek()>32) s+=char(get()); return s; }
    long long integer() {
        int c=space(); bool negative=false;
        if (c=='-' || c=='+') { negative=c=='-'; get(); c=peek(); }
        if (c<'0' || c>'9') throw std::runtime_error("expected integer in DIMACS");
        U64 value=0;
        while ((c=peek())>='0' && c<='9') {
            unsigned digit=unsigned(get()-'0');
            if (value>(U64(std::numeric_limits<long long>::max())-digit)/10)
                throw std::runtime_error("integer overflow in DIMACS");
            value=value*10+digit;
        }
        if (c!=EOF && c>32) throw std::runtime_error("invalid character after DIMACS integer");
        return negative ? -static_cast<long long>(value) : static_cast<long long>(value);
    }
};
static CNF read_cnf(const std::string& path, int& n, const Budget& budget) {
    FastInput in(path); CNF clauses; Clause pending;
    bool header=false; U64 declared=0;
    for (;;) {
        int c=in.space(); if (c==EOF) break;
        if (c=='c') { in.line(); continue; }
        if (c=='p') {
            if (header) throw std::runtime_error("duplicate DIMACS header");
            in.get(); if (in.word()!="cnf") throw std::runtime_error("expected p cnf header");
            long long nv=in.integer(), nc=in.integer();
            if (nv<0 || nc<0 || nv>(std::numeric_limits<int>::max()-2)/2)
                throw std::runtime_error("DIMACS size outside 32-bit literal representation");
            n=static_cast<int>(nv); declared=static_cast<U64>(nc); header=true;
            while ((c=in.peek())!=EOF && c!='\n') {
                if (c>32) throw std::runtime_error("extra text in DIMACS header");
                in.get();
            }
            continue;
        }
        if (!header) throw std::runtime_error("missing p cnf header");
        long long literal=in.integer();
        if (!literal) {
            clauses.push_back(std::move(pending)); pending.clear();
            if (clauses.size()>declared) throw std::runtime_error("too many DIMACS clauses");
            if (!(clauses.size()&4095)) budget.check();
        } else {
            U64 v=static_cast<U64>(literal<0 ? -literal : literal);
            if (v>static_cast<U64>(n)) throw std::runtime_error("literal exceeds declared variable count");
            pending.push_back(2*int(v-1)+(literal<0));
        }
    }
    if (!header || !pending.empty() || clauses.size()!=declared)
        throw std::runtime_error("missing header, unfinished clause, or clause-count mismatch");
    budget.check(); return clauses;
}
static int value(Lit p, const std::vector<std::int8_t>& values) {
    int x=values[var(p)]; return x<0 ? -1 : x^(p&1);
}
static bool verify(const CNF& clauses, const std::vector<std::int8_t>& values) {
    for (const auto& clause:clauses) {
        bool satisfied=false;
        for (Lit p:clause) if (value(p,values)==1) { satisfied=true; break; }
        if (!satisfied) return false;
    }
    return true;
}

// Linear occurrence-driven level-zero propagation. Literal XOR identifies
// the sole remaining literal without rescanning long clauses.
static bool simplify(const CNF& original, int n, CNF& residual,
                     std::vector<std::int8_t>& model, Stats& stats, const Budget& budget) {
    CNF clauses; clauses.reserve(original.size());
    for (const auto& source:original) {
        Clause c=source; std::sort(c.begin(),c.end());
        c.erase(std::unique(c.begin(),c.end()),c.end());
        bool tautology=false;
        for (std::size_t j=1;j<c.size();++j) if (c[j]==(c[j-1]^1)) { tautology=true; break; }
        if (tautology) continue;
        if (c.empty()) return false;
        clauses.push_back(std::move(c));
        if (!(clauses.size()&4095)) budget.check();
    }
    std::vector<std::vector<int>> occurrences(std::size_t(n)*2);
    std::vector<int> remaining(clauses.size()), xr(clauses.size());
    std::vector<char> satisfied(clauses.size(),0);
    std::vector<Lit> trail;
    auto assign=[&](Lit p) {
        int v=var(p), b=1^(p&1);
        if (model[v]>=0) return model[v]==b;
        model[v]=std::int8_t(b); trail.push_back(p); ++stats.root_assignments; return true;
    };
    for (std::size_t c=0;c<clauses.size();++c) {
        remaining[c]=int(clauses[c].size());
        for (Lit p:clauses[c]) { occurrences[p].push_back(int(c)); xr[c]^=p; }
        if (clauses[c].size()==1 && !assign(clauses[c][0])) return false;
    }
    for (std::size_t head=0;head<trail.size();++head) {
        Lit p=trail[head];
        for (int c:occurrences[p]) satisfied[c]=1;
        for (int c:occurrences[p^1]) if (!satisfied[c]) {
            --remaining[c]; xr[c]^=(p^1);
            if (!remaining[c] || (remaining[c]==1 && !assign(xr[c]))) return false;
        }
        if (!(head&4095)) budget.check();
    }
    residual.reserve(clauses.size());
    for (std::size_t c=0;c<clauses.size();++c) if (!satisfied[c]) {
        Clause out; out.reserve(remaining[c]);
        for (Lit p:clauses[c]) if (model[var(p)]<0) out.push_back(p);
        if (out.empty()) return false;
        residual.push_back(std::move(out));
    }
    budget.check(); return true;
}

struct DSU {
    std::vector<int> parent, size;
    explicit DSU(int n):parent(n),size(n,1) { std::iota(parent.begin(),parent.end(),0); }
    int find(int x) { while (parent[x]!=x) { parent[x]=parent[parent[x]]; x=parent[x]; } return x; }
    void join(int a,int b) { a=find(a);b=find(b); if(a==b)return; if(size[a]<size[b])std::swap(a,b);parent[b]=a;size[a]+=size[b]; }
};
struct Component { std::vector<int> variables, clauses; };
static std::vector<Component> components(const CNF& clauses,int n,const Budget& budget) {
    DSU dsu(n); std::vector<char> used(n,0);
    for (const auto& c:clauses) for (Lit p:c) { used[var(p)]=1; dsu.join(var(c[0]),var(p)); }
    std::vector<Component> parts; std::vector<int> id(n,-1);
    for (int v=0;v<n;++v) if (used[v]) {
        int root=dsu.find(v);
        if (id[root]<0) { id[root]=int(parts.size()); parts.emplace_back(); }
        parts[id[root]].variables.push_back(v);
    }
    for (std::size_t c=0;c<clauses.size();++c)
        parts[id[dsu.find(var(clauses[c][0]))]].clauses.push_back(int(c));
    std::stable_sort(parts.begin(),parts.end(),[](const Component&a,const Component&b){return a.variables.size()<b.variables.size();});
    budget.check(); return parts;
}

// Iterative Kosaraju using compact CSR adjacency: no recursive DFS stack.
static bool solve_2sat(const CNF& clauses,int n,std::vector<std::int8_t>& model,const Budget& budget) {
    const int nodes=2*n; std::vector<std::pair<int,int>> edges; edges.reserve(2*clauses.size());
    for (const auto& c:clauses) {
        Lit a=c[0],b=c.size()==1?a:c[1]; edges.emplace_back(a^1,b);edges.emplace_back(b^1,a);
    }
    std::vector<std::size_t> offset(nodes+1), reverse_offset(nodes+1);
    for (auto e:edges) {++offset[e.first+1];++reverse_offset[e.second+1];}
    std::partial_sum(offset.begin(),offset.end(),offset.begin());
    std::partial_sum(reverse_offset.begin(),reverse_offset.end(),reverse_offset.begin());
    std::vector<int> adj(edges.size()),rev(edges.size());
    auto at=offset, rat=reverse_offset;
    for (auto e:edges) {adj[at[e.first]++]=e.second;rev[rat[e.second]++]=e.first;}
    std::vector<char> seen(nodes,0);std::vector<int> order;order.reserve(nodes);
    std::vector<std::pair<int,std::size_t>> stack;
    for (int root=0;root<nodes;++root) if(!seen[root]) {
        seen[root]=1;stack.emplace_back(root,offset[root]);
        while(!stack.empty()) {
            int v=stack.back().first; auto& next=stack.back().second;
            if(next<offset[v+1]) {int u=adj[next++];if(!seen[u]){seen[u]=1;stack.emplace_back(u,offset[u]);}}
            else {order.push_back(v);stack.pop_back();}
        }
        if (!(root&4095))budget.check();
    }
    std::vector<int> group(nodes,-1), todo; int counter=0;
    for(auto it=order.rbegin();it!=order.rend();++it) if(group[*it]<0) {
        todo.push_back(*it);group[*it]=counter;
        while(!todo.empty()) {int v=todo.back();todo.pop_back();for(auto k=reverse_offset[v];k<reverse_offset[v+1];++k){int u=rev[k];if(group[u]<0){group[u]=counter;todo.push_back(u);}}}
        ++counter;
        if (!(counter&4095))budget.check();
    }
    model.resize(n);
    for(int v=0;v<n;++v){if(group[2*v]==group[2*v+1])return false;model[v]=group[2*v]>group[2*v+1];}
    budget.check();return true;
}

// Greedy ordering minimizes the immediate change in live clauses. Scores
// update when a clause opens or becomes closable; total incidence work is O(L).
struct OrderInfo {std::vector<int> order;int width=0, original_width=0;};
static int order_width(const CNF& clauses,const std::vector<int>& order) {
    int n=int(order.size());std::vector<int> position(n),start(n),end(n);
    for(int i=0;i<n;++i)position[order[i]]=i;
    for(const auto& c:clauses){int a=n,b=-1;for(Lit p:c){a=std::min(a,position[var(p)]);b=std::max(b,position[var(p)]);}++start[a];++end[b];}
    int active=0,width=0;for(int i=0;i<n;++i){active+=start[i];width=std::max(width,active);active-=end[i];}return width;
}
static OrderInfo choose_order(const CNF& clauses,int n,const Budget& budget) {
    std::vector<std::vector<int>> occurs(n);std::vector<int> left(clauses.size()),score(n),touch(n),version(n);
    std::vector<char> opened(clauses.size(),0),removed(n,0);
    for(std::size_t c=0;c<clauses.size();++c){left[c]=int(clauses[c].size());for(Lit p:clauses[c]){occurs[var(p)].push_back(int(c));if(left[c]>1)++score[var(p)];}}
    struct Item{int score,touch,degree,v,version;};
    struct Worse{bool operator()(const Item&a,const Item&b)const{
        if(a.score!=b.score)return a.score>b.score;
        if(a.touch!=b.touch)return a.touch<b.touch;
        if(a.degree!=b.degree)return a.degree>b.degree;
        return a.v>b.v;
    }};
    std::priority_queue<Item,std::vector<Item>,Worse> heap;
    auto push=[&](int v){heap.push({score[v],touch[v],int(occurs[v].size()),v,version[v]});};
    for(int v=0;v<n;++v)push(v);
    OrderInfo result;result.order.reserve(n);
    while(!heap.empty()) {
        Item item=heap.top();heap.pop();int v=item.v;
        if(removed[v]||item.version!=version[v])continue;
        removed[v]=1;result.order.push_back(v);
        for(int c:occurs[v]) {
            --left[c];
            if(!opened[c]){opened[c]=1;for(Lit p:clauses[c]){int u=var(p);if(!removed[u]){--score[u];++touch[u];++version[u];push(u);}}}
            if(left[c]==1)for(Lit p:clauses[c]){int u=var(p);if(!removed[u]){--score[u];++version[u];push(u);break;}}
        }
        if(!(result.order.size()&4095))budget.check();
    }
    std::vector<int> original(n);std::iota(original.begin(),original.end(),0);
    result.original_width=order_width(clauses,original);result.width=order_width(clauses,result.order);
    if(result.original_width<result.width){result.order=std::move(original);result.width=result.original_width;}
    return result;
}

// Flat hash table: exact equality, linear probing, no node allocation.
struct FlatIndex {
    std::vector<U64> keys,ids;std::size_t mask;
    explicit FlatIndex(std::size_t maximum){std::size_t capacity=8;while(capacity<maximum*2+1)capacity<<=1;keys.resize(capacity);ids.assign(capacity,0);mask=capacity-1;}
    bool insert(U64 key,U64 id){U64 h=key+0x9e3779b97f4a7c15ULL;h=(h^(h>>30))*0xbf58476d1ce4e5b9ULL;h=(h^(h>>27))*0x94d049bb133111ebULL;h^=h>>31;std::size_t slot=h&mask;
        while(ids[slot]){if(keys[slot]==key)return false;slot=(slot+1)&mask;}keys[slot]=key;ids[slot]=id+1;return true;}
};
// Return 1 SAT, 0 UNSAT, -1 exact fallback requested; no branch is discarded
// and called UNSAT merely because a graph budget was reached.
static int solve_graph(const CNF& clauses,int n,const OrderInfo& ordering,
                       std::vector<std::int8_t>& model,const Options& options,Stats& stats,const Budget& budget) {
    const auto& order=ordering.order;std::vector<int> position(n),first(clauses.size()),last(clauses.size());
    for(int i=0;i<n;++i)position[order[i]]=i;
    std::vector<std::vector<int>> starts(n),ends(n);
    for(std::size_t c=0;c<clauses.size();++c){int a=n,b=-1;for(Lit p:clauses[c]){a=std::min(a,position[var(p)]);b=std::max(b,position[var(p)]);}first[c]=a;last[c]=b;starts[a].push_back(int(c));ends[b].push_back(int(c));}
    std::vector<U64> positive(n),negative(n),close(n);std::vector<int> free,slot(clauses.size());int next=0;
    for(int i=0;i<n;++i){for(int c:starts[i]){int k;if(free.empty())k=next++;else{k=free.back();free.pop_back();}slot[c]=k;assert(k<64);U64 bit=U64(1)<<k;
        for(Lit p:clauses[c])((p&1)?negative:positive)[position[var(p)]]|=bit;
        close[last[c]]|=bit;}
        for(int c:ends[i])free.push_back(slot[c]);}
    std::vector<U64> current{0};std::vector<std::vector<U64>> parents;parents.reserve(n);
    U64 local_nodes=1;++stats.graph_nodes;stats.peak_frontier=std::max<U64>(stats.peak_frontier,1);
    for(int i=0;i<n;++i){budget.check();FlatIndex index(current.size()*2);std::vector<U64> following,links;following.reserve(std::min<U64>(2*current.size(),options.frontier_budget));links.reserve(following.capacity());
        for(std::size_t p=0;p<current.size();++p)for(int b=0;b<=1;++b){U64 hit=current[p]|(b?positive[i]:negative[i]);if((hit&close[i])!=close[i])continue;U64 child=hit^close[i];
            if(!index.insert(child,following.size()))continue;
            if(following.size()>=options.frontier_budget||local_nodes>=options.node_budget){++stats.graph_fallbacks;return -1;}
            following.push_back(child);links.push_back((U64(p)<<1)|b);++local_nodes;++stats.graph_nodes;
            stats.peak_frontier=std::max<U64>(stats.peak_frontier,following.size());
        }
        if(following.empty())return 0;
        current=std::move(following);parents.push_back(std::move(links));
    }
    model.assign(n,0);std::size_t index=0;
    for(int i=n-1;i>=0;--i){U64 link=parents[i][index];model[order[i]]=link&1;index=link>>1;}
    return 1;
}

// Conflict-driven search: one trail, watched literals, binary implication lists,
// first-UIP learning, indexed EVSIDS heap, phase saving, Luby restarts, and
// learned-clause reduction/compaction. No breadth-first frontier is retained.
class CDCL {
    struct Record {Clause lits;bool learned=false;int lbd=0;U64 touched=0;};
    struct Watch {int clause;Lit blocker;};
    struct Binary {Lit other;int clause;};
    int n;
    const Options& options;Stats& stats;const Budget& budget;
    std::vector<Record> db;
    std::vector<std::vector<Watch>> watches;
    std::vector<std::vector<Binary>> binary;
    std::vector<std::int8_t> values,phase;
    std::vector<int> level,reason,heap,position;
    std::vector<double> activity;
    std::vector<char> seen;
    std::vector<Lit> trail;
    std::vector<std::size_t> limits;
    std::size_t qhead=0;
    double increment=1;
    bool okay=true;
    U64 local_conflicts=0,next_reduce;

    bool better(int a,int b)const{return activity[a]!=activity[b]?activity[a]>activity[b]:a<b;}
    void up(int i){while(i>0){int p=(i-1)/2;if(!better(heap[i],heap[p]))break;std::swap(heap[i],heap[p]);position[heap[i]]=i;position[heap[p]]=p;i=p;}}
    void down(int i){for(;;){int l=2*i+1;if(l>=int(heap.size()))break;int r=l+1,j=(r<int(heap.size())&&better(heap[r],heap[l]))?r:l;if(!better(heap[j],heap[i]))break;std::swap(heap[j],heap[i]);position[heap[j]]=j;position[heap[i]]=i;i=j;}}
    void insert(int v){if(position[v]>=0)return;position[v]=int(heap.size());heap.push_back(v);up(position[v]);}
    int pop(){int v=heap[0];position[v]=-1;int last=heap.back();heap.pop_back();if(!heap.empty()){heap[0]=last;position[last]=0;down(0);}return v;}
    void bump(int v){activity[v]+=increment;if(activity[v]>1e100){for(auto& a:activity)a*=1e-100;increment*=1e-100;}if(position[v]>=0)up(position[v]);}
    bool enqueue(Lit p,int from){int v=var(p),b=1^(p&1);if(values[v]>=0)return values[v]==b;values[v]=std::int8_t(b);phase[v]=std::int8_t(b);level[v]=int(limits.size());reason[v]=from;trail.push_back(p);return true;}
    void attach(int id){const auto& c=db[id].lits;if(c.size()==2){binary[c[0]^1].push_back({c[1],id});binary[c[1]^1].push_back({c[0],id});}
        else if(c.size()>2){watches[c[0]^1].push_back({id,c[1]});watches[c[1]^1].push_back({id,c[0]});}}
    int add(Clause c,bool learned=false,int lbd=0){int id=int(db.size());db.push_back({std::move(c),learned,lbd,local_conflicts});attach(id);return id;}
    int propagate(){
        while(qhead<trail.size()){
            Lit p=trail[qhead++];++stats.propagations;
            if(!(stats.propagations&2047))budget.check();
            for(const auto& edge:binary[p]){int val=value(edge.other,values);if(val==0)return edge.clause;if(val<0)enqueue(edge.other,edge.clause);}
            auto& list=watches[p];std::size_t write=0;
            for(std::size_t read=0;read<list.size();++read){Watch w=list[read];
                if(value(w.blocker,values)==1){list[write++]=w;continue;}
                auto& c=db[w.clause].lits;Lit false_lit=p^1;
                if(c[0]==false_lit)std::swap(c[0],c[1]);
                assert(c[1]==false_lit);Lit other=c[0];
                if(value(other,values)==1){list[write++]={w.clause,other};continue;}
                bool moved=false;
                for(std::size_t k=2;k<c.size();++k)if(value(c[k],values)!=0){Lit replacement=c[k];c[1]=replacement;c[k]=false_lit;watches[replacement^1].push_back({w.clause,other});moved=true;break;}
                if(moved)continue;
                list[write++]={w.clause,other};
                if(value(other,values)==0){for(++read;read<list.size();++read)list[write++]=list[read];list.resize(write);return w.clause;}
                enqueue(other,w.clause);
            }
            list.resize(write);
        }
        return -1;
    }
    void cancel(int target){if(int(limits.size())<=target)return;std::size_t boundary=limits[target];
        for(std::size_t i=trail.size();i>boundary;--i){int v=var(trail[i-1]);values[v]=-1;reason[v]=-1;level[v]=0;insert(v);}
        trail.resize(boundary);limits.resize(target);qhead=std::min(qhead,boundary);
    }
    Clause analyze(int conflict,int& back,int& lbd){Clause learned(1,-1);std::vector<int> touched;int pending=0;Lit p=-1;std::size_t cursor=trail.size();
        do{if(conflict<0)throw std::runtime_error("internal missing conflict reason");auto& record=db[conflict];record.touched=local_conflicts;
            for(Lit q:record.lits){int v=var(q);if((p>=0&&v==var(p))||seen[v]||level[v]==0)continue;seen[v]=1;touched.push_back(v);bump(v);if(level[v]==int(limits.size()))++pending;else learned.push_back(q);}
            do{if(!cursor)throw std::runtime_error("internal invalid implication graph");p=trail[--cursor];}while(!seen[var(p)]);
            seen[var(p)]=0;--pending;conflict=reason[var(p)];
        }while(pending>0);
        learned[0]=p^1;back=0;
        if(learned.size()>1){std::size_t highest=1;for(std::size_t i=2;i<learned.size();++i)if(level[var(learned[i])]>level[var(learned[highest])])highest=i;std::swap(learned[1],learned[highest]);back=level[var(learned[1])];}
        std::vector<int> levels;levels.reserve(learned.size());for(Lit q:learned)levels.push_back(level[var(q)]);std::sort(levels.begin(),levels.end());lbd=int(std::unique(levels.begin(),levels.end())-levels.begin());
        for(int v:touched)seen[v]=0;
        return learned;
    }
    static U64 luby(U64 index){U64 size=1,sequence=0;while(size<index+1){size=2*size+1;++sequence;}while(size-1!=index){size=(size-1)/2;--sequence;index%=size;}return U64(1)<<sequence;}
    void reduce(){
        assert(limits.empty());++stats.reductions;
        std::vector<char> locked(db.size(),0),remove(db.size(),0);for(int r:reason)if(r>=0)locked[r]=1;
        std::vector<int> candidates;
        for(std::size_t id=0;id<db.size();++id)if(db[id].learned&&db[id].lits.size()>2&&db[id].lbd>2&&!locked[id])candidates.push_back(int(id));
        std::sort(candidates.begin(),candidates.end(),[&](int a,int b){if(db[a].lbd!=db[b].lbd)return db[a].lbd>db[b].lbd;if(db[a].touched!=db[b].touched)return db[a].touched<db[b].touched;return a<b;});
        std::size_t count=candidates.size()/2;if(!count)return;
        for(std::size_t i=0;i<count;++i)remove[candidates[i]]=1;
        std::vector<int> remap(db.size(),-1);std::vector<Record> compact;compact.reserve(db.size()-count);
        for(std::size_t id=0;id<db.size();++id)if(!remove[id]){remap[id]=int(compact.size());compact.push_back(std::move(db[id]));}
        for(auto& r:reason)if(r>=0){assert(remap[r]>=0);r=remap[r];}
        db=std::move(compact);for(auto& w:watches)w.clear();for(auto& b:binary)b.clear();for(std::size_t id=0;id<db.size();++id)attach(int(id));
        stats.learned_deleted+=count;++stats.garbage_collections;budget.check();
    }
public:
    CDCL(const CNF& clauses,int nv,const Options& o,Stats& s,const Budget& b):n(nv),options(o),stats(s),budget(b),watches(2*nv),binary(2*nv),values(nv,-1),phase(nv,0),level(nv,0),reason(nv,-1),position(nv,-1),activity(nv,0),seen(nv,0),next_reduce(o.reduce_every){
        db.reserve(clauses.size()+1024);std::vector<double> positive(n),negative(n);
        for(const auto& c:clauses){if(c.empty()){okay=false;break;}int id=add(c);if(c.size()==1&&!enqueue(c[0],id))okay=false;double weight=std::ldexp(1.0,-std::min<int>(int(c.size()),20));for(Lit p:c){activity[var(p)]+=weight;((p&1)?negative:positive)[var(p)]+=weight;}}
        for(int v=0;v<n;++v){if(values[v]<0)phase[v]=positive[v]>=negative[v];insert(v);}
    }
    bool solve(std::vector<std::int8_t>& model){if(!okay)return false;U64 restart_index=0,restart_limit=options.restart_base,since_restart=0;
        for(;;){budget.check();int conflict=propagate();
            if(conflict>=0){++stats.conflicts;++local_conflicts;++since_restart;if(limits.empty())return false;
                int back,lbd;Clause learned=analyze(conflict,back,lbd);cancel(back);Lit asserting=learned[0];int id=add(std::move(learned),true,lbd);++stats.learned_created;if(!enqueue(asserting,id))throw std::runtime_error("internal nonasserting learned clause");increment/=0.95;
                if(since_restart>=restart_limit){cancel(0);++stats.restarts;since_restart=0;restart_limit=options.restart_base*luby(++restart_index);
                    if(local_conflicts>=next_reduce){reduce();next_reduce=local_conflicts+options.reduce_every;}}
            }else{
                int v=-1;while(!heap.empty()){int candidate=pop();if(values[candidate]<0){v=candidate;break;}}
                if(v<0){model=values;return true;}
                limits.push_back(trail.size());++stats.decisions;enqueue(2*v+(phase[v]?0:1),-1);
            }
        }
    }
};

static bool solve_all(const CNF& original,int n,std::vector<std::int8_t>& model,const Options& options,Stats& stats,const Budget& budget) {
    auto prep=Clock::now();model.assign(n,-1);CNF residual;
    if(!simplify(original,n,residual,model,stats,budget)){stats.preprocess_seconds=elapsed(prep);return false;}
    auto parts=components(residual,n,budget);stats.components=parts.size();stats.residual_clauses=residual.size();
    for(const auto& p:parts)stats.residual_variables+=p.variables.size();
    std::vector<int> local(n,-1);stats.preprocess_seconds=elapsed(prep);auto search=Clock::now();
    for(const auto& component:parts){budget.check();int size=int(component.variables.size());for(int i=0;i<size;++i)local[component.variables[i]]=i;
        CNF cnf;cnf.reserve(component.clauses.size());bool binary=true;
        for(int id:component.clauses){Clause c;c.reserve(residual[id].size());for(Lit p:residual[id])c.push_back(2*local[var(p)]+(p&1));if(c.size()>2)binary=false;cnf.push_back(std::move(c));}
        std::vector<std::int8_t> assignment;int answer=-1;
        if(options.engine=="auto"&&binary){++stats.two_sat_components;answer=solve_2sat(cnf,size,assignment,budget)?1:0;}
        else if(options.engine=="auto"&&options.dp_width>0){auto ordering=choose_order(cnf,size,budget);stats.original_width_max=std::max<U64>(stats.original_width_max,ordering.original_width);stats.ordered_width_max=std::max<U64>(stats.ordered_width_max,ordering.width);
            if(ordering.width<=options.dp_width){++stats.graph_components;answer=solve_graph(cnf,size,ordering,assignment,options,stats,budget);}}
        if(answer<0){++stats.cdcl_components;CDCL solver(cnf,size,options,stats,budget);answer=solver.solve(assignment)?1:0;}
        if(!answer){stats.search_seconds=elapsed(search);return false;}
        for(int i=0;i<size;++i)model[component.variables[i]]=assignment[i];
        ++stats.solved_components;
    }
    for(auto& b:model)if(b<0)b=0;
    stats.search_seconds=elapsed(search);return true;
}
static long peak_rss(){std::ifstream file("/proc/self/status");std::string line;while(std::getline(file,line))if(line.rfind("VmHWM:",0)==0){std::istringstream in(line.substr(6));long kib;if(in>>kib)return kib;}return -1;}
static std::string json(const std::string& status,const Stats&s,const std::vector<std::int8_t>& model,bool verified){std::ostringstream out;out<<std::setprecision(12)<<"{\"engine\":\"polylog_ultra\",\"status\":\""<<status<<"\",\"verified\":"<<(verified?"true":"null")<<",\"peak_rss_kib\":"<<peak_rss();
#define FIELD(name) out<<",\"" #name "\":"<<s.name
    FIELD(variables);FIELD(clauses);FIELD(residual_variables);FIELD(residual_clauses);FIELD(root_assignments);FIELD(components);FIELD(solved_components);
    FIELD(two_sat_components);FIELD(graph_components);FIELD(graph_fallbacks);FIELD(graph_nodes);FIELD(peak_frontier);FIELD(original_width_max);FIELD(ordered_width_max);
    FIELD(cdcl_components);FIELD(decisions);FIELD(conflicts);FIELD(propagations);FIELD(learned_created);FIELD(learned_deleted);FIELD(reductions);FIELD(garbage_collections);FIELD(restarts);
    FIELD(parse_seconds);FIELD(preprocess_seconds);FIELD(search_seconds);FIELD(verify_seconds);FIELD(total_seconds);
#undef FIELD
    out<<",\"witness\":";if(status!="SAT")out<<"null";else{out<<'[';for(std::size_t i=0;i<model.size();++i){if(i)out<<',';out<<int(model[i]);}out<<']';}out<<'}';return out.str();}
static void sat_output(std::ostream& out,const std::string& status,const Stats&stats,const std::vector<std::int8_t>& model){
    out<<"c POLYLOG ULTRA: configuration graphs + 2-SAT + CDCL\n"<<"c seconds "<<std::setprecision(9)<<stats.total_seconds<<" conflicts "<<stats.conflicts<<" decisions "<<stats.decisions<<" peak_frontier "<<stats.peak_frontier<<'\n';
    out<<"s "<<(status=="SAT"?"SATISFIABLE":status=="UNSAT"?"UNSATISFIABLE":"UNKNOWN")<<'\n';
    if(status=="SAT"){out<<"v";for(std::size_t i=0;i<model.size();++i){if(i&&i%20==0)out<<"\nv";long long literal=static_cast<long long>(i)+1;out<<' '<<(model[i]?literal:-literal);}out<<" 0\n";}
}
static U64 number(const std::string& s){if(s.empty()||s[0]=='-')throw std::runtime_error("expected nonnegative integer");std::size_t used;U64 v=std::stoull(s,&used);if(used!=s.size())throw std::runtime_error("invalid number");return v;}
int main(int argc,char**argv){
    try{
        Options options;int begin=1;
        if(argc>1&&std::string(argv[1])=="cnf")begin=2;
        if(argc<=begin||std::string(argv[begin])=="--help"){
            std::cout<<"Usage: solver_ultra input.cnf [--output result.txt] [--json] [--stats stats.json]\n"
                     <<"  --engine auto|cdcl  --timeout SECONDS (0=unlimited)\n"
                     <<"  --dp-width 0..64 (default 18)  --frontier-budget N (4096)\n"
                     <<"  --node-budget N (262144)  --restart-base N (100)  --reduce-every N (2000)\n"
                     <<"Graph budgets select exact CDCL fallback; they never imply UNSAT.\n";return 0;
        }
        options.input=argv[begin++];
        for(int i=begin;i<argc;++i){std::string flag=argv[i];if(flag=="--json"){options.json=true;continue;}if(i+1==argc)throw std::runtime_error("missing option value");std::string arg=argv[++i];
            if(flag=="--output")options.output=arg;else if(flag=="--stats")options.stats_file=arg;else if(flag=="--engine")options.engine=arg;
            else if(flag=="--timeout"){std::size_t used;options.timeout=std::stod(arg,&used);if(used!=arg.size()||!std::isfinite(options.timeout)||options.timeout<0)throw std::runtime_error("invalid timeout");}
            else if(flag=="--dp-width"){U64 width=number(arg);if(width>64)throw std::runtime_error("DP specialization supports width 0..64; wider components use CDCL");options.dp_width=int(width);}
            else if(flag=="--frontier-budget")options.frontier_budget=number(arg);else if(flag=="--node-budget")options.node_budget=number(arg);
            else if(flag=="--restart-base")options.restart_base=number(arg);else if(flag=="--reduce-every")options.reduce_every=number(arg);else throw std::runtime_error("unknown option: "+flag);
        }
        if(options.engine!="auto"&&options.engine!="cdcl")throw std::runtime_error("engine must be auto or cdcl");
        if(!options.frontier_budget||!options.node_budget||!options.restart_base||!options.reduce_every)throw std::runtime_error("budgets and intervals must be positive");
        if(options.output==options.input||options.stats_file==options.input||(!options.output.empty()&&options.output==options.stats_file))throw std::runtime_error("input and output paths must differ");
        Budget budget;budget.limit=options.timeout;Stats stats;std::vector<std::int8_t> model;std::string status="UNKNOWN";bool verified=false;
        try{int n=0;auto parse=Clock::now();CNF original=read_cnf(options.input,n,budget);stats.parse_seconds=elapsed(parse);stats.variables=n;stats.clauses=original.size();
            bool sat=solve_all(original,n,model,options,stats,budget);status=sat?"SAT":"UNSAT";
            if(sat){auto check=Clock::now();if(!verify(original,model))throw std::runtime_error("internal error: invalid SAT witness");stats.verify_seconds=elapsed(check);verified=true;}
        }catch(const TimeLimit&){status="UNKNOWN";}
        stats.total_seconds=elapsed(budget.start);std::string record=json(status,stats,model,verified);
        if(options.json)std::cout<<record<<'\n';else sat_output(std::cout,status,stats,model);
        if(!options.output.empty()){std::ofstream file(options.output);if(!file)throw std::runtime_error("cannot open output file");sat_output(file,status,stats,model);if(!file)throw std::runtime_error("failed to write output");}
        if(!options.stats_file.empty()){std::ofstream file(options.stats_file);if(!(file<<record<<'\n'))throw std::runtime_error("cannot write stats file");}
        return 0;
    }catch(const std::exception& error){std::cerr<<"error: "<<error.what()<<'\n';return 2;}
}
