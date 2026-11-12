#include <bits/stdc++.h>
using namespace std;
using U64 = uint64_t;
int W=9,H=7,K=5,TARGET=1;
static const uint32_t INF = 1000000000u;
struct Entry { U64 x=0, o=0; uint32_t pn=1, dn=1; int8_t best=-1; uint8_t flags=0; uint16_t pad=0; };
struct TT {
  vector<Entry> t; uint64_t mask, used=0, hits=0, stores=0;
  TT(size_t pow=20):t((1ull<<pow)*4),mask((1ull<<pow)-1){}
  static U64 mirror(U64 m){
    U64 out=0; const U64 colmask=(1ull<<H)-1;
    for(int c=0;c<W;c++) out |= ((m>>(c*H))&colmask)<<((W-1-c)*H);
    return out;
  }
  static void canon(U64 &x,U64 &o){
    U64 mx=mirror(x), mo=mirror(o);
    if(mx<x || (mx==x && mo<o)){x=mx;o=mo;}
  }
  static uint64_t hash(U64 x,U64 o){
    uint64_t z=x+0x9e3779b97f4a7c15ULL; z=(z^(z>>30))*0xbf58476d1ce4e5b9ULL; z=(z^(z>>27))*0x94d049bb133111ebULL; z^=z>>31;
    uint64_t q=o+0x517cc1b727220a95ULL; q=(q^(q>>30))*0xbf58476d1ce4e5b9ULL; q=(q^(q>>27))*0x94d049bb133111ebULL; q^=q>>31;
    return z ^ ((q<<23)|(q>>41));
  }
  Entry* find(U64 x,U64 o) {
    canon(x,o); size_t base=(hash(x,o)&mask)*4;
    for(int j=0;j<4;j++){Entry &e=t[base+j];if(e.flags && e.x==x && e.o==o){hits++;return &e;}}
    return nullptr;
  }
  Entry& get(U64 x,U64 o) {
    canon(x,o); size_t base=(hash(x,o)&mask)*4; Entry *empty=nullptr,*victim=nullptr;
    for(int j=0;j<4;j++){
      Entry &e=t[base+j];
      if(e.flags && e.x==x && e.o==o)return e;
      if(!e.flags && !empty)empty=&e;
      if(e.flags && e.pn!=0 && e.dn!=0 && (!victim || min(e.pn,e.dn)>min(victim->pn,victim->dn)))victim=&e;
    }
    Entry *e=empty?empty:(victim?victim:&t[base]);
    if(!e->flags)used++;
    e->x=x;e->o=o;e->pn=1;e->dn=1;e->best=-1;e->flags=1;return *e;
  }
  void set(U64 x,U64 o,uint32_t pn,uint32_t dn,int best=-1){ Entry &e=get(x,o); e.pn=pn;e.dn=dn;if(best>=0)e.best=(int8_t)best; stores++; }
};
uint64_t NODE_LIMIT=0, nodes=0; chrono::steady_clock::time_point startTime; double timeLimit=0;
TT *tt; U64 rootX=0, rootO=0; int rootPly=0;
inline bool hasWin(U64 bits,int c,int r){
 const int dc[4]={1,0,1,1}, dr[4]={0,1,1,-1};
 for(int d=0;d<4;d++) { int count=1; for(int s=1;s<K;s++){int x=c+dc[d]*s,y=r+dr[d]*s;if(x<0||x>=W||y<0||y>=H||!(bits&(1ull<<(x*H+y))))break;count++;} for(int s=1;count<K && s<K;s++){int x=c-dc[d]*s,y=r-dr[d]*s;if(x<0||x>=W||y<0||y>=H||!(bits&(1ull<<(x*H+y))))break;count++;} if(count>=K)return true; }
 return false;
}
inline uint32_t satAdd(uint32_t a,uint32_t b){uint64_t s=(uint64_t)a+b;return s>=INF?INF:(uint32_t)s;}
inline uint32_t satSub(uint32_t a,uint32_t b){return a>b?a-b:1;}
inline uint32_t plusOne(uint32_t a){return a>=INF?INF:a+1;}
struct Move { U64 x,o; int col,row; bool terminal; uint32_t pn,dn; };
vector<int> orderCols(const array<int,16>& ht) { vector<int> cs; for(int c=0;c<W;c++)if(ht[c]<H)cs.push_back(c); stable_sort(cs.begin(),cs.end(),[](int a,int b){double ca=abs(a-(W-1)/2.0),cb=abs(b-(W-1)/2.0); return ca<cb;}); return cs; }
// target is player 1/X. Return exact proof/disproof numbers when thresholds settle.
void dfpn(U64 x,U64 o,array<int,16>& ht,int ply,uint32_t phi,uint32_t delta) {
 nodes++;
 if((NODE_LIMIT && nodes>=NODE_LIMIT) || (timeLimit>0 && (nodes%200000==0) && chrono::duration<double>(chrono::steady_clock::now()-startTime).count()>timeLimit)) return;
 Entry &here0=tt->get(x,o); // Note don't retain reference across recursive calls due fixed table aliasing.
 (void)here0;
 while(true) {
   Entry *ep=tt->find(x,o); uint32_t pn=ep?ep->pn:1, dn=ep?ep->dn:1; int oldBest=ep?ep->best:-1;
   if(pn==0 || dn==0 || pn>=phi || dn>=delta) return;
   bool xTurn = (ply%2==0); // ply from initial position, X moves first
   bool isOR = (xTurn == (TARGET==1)); // target player seeks win
   vector<Move> moves; moves.reserve(W);
   auto cols=orderCols(ht);
   uint32_t nodePN = isOR ? INF : 0, nodeDN = isOR ? 0 : INF;
   for(int c:cols){
     int r=ht[c]; U64 b=1ull<<(c*H+r); U64 nx=x, no=o;
     if(xTurn) nx|=b; else no|=b;
     bool win=hasWin(xTurn?nx:no,c,r);
     bool full=(ply+1==W*H);
     uint32_t cp=1,cd=1;
     if(win || full) {
       bool targetWins = win && (xTurn == (TARGET==1));
       if(targetWins){cp=0;cd=INF;} else {cp=INF;cd=0;}
       tt->set(nx,no,cp,cd);
     } else if(Entry *ce=tt->find(nx,no)){cp=ce->pn;cd=ce->dn;}
     if(isOR){ nodePN=min(nodePN,cp); nodeDN=satAdd(nodeDN,cd); }
     else { nodePN=satAdd(nodePN,cp); nodeDN=min(nodeDN,cd); }
     moves.push_back({nx,no,c,r,win||full,cp,cd});
   }
   if(moves.empty()) { tt->set(x,o,INF,0); return; }
   // Store updated aggregation before selecting child; selected children are refreshed on next pass.
   tt->set(x,o,nodePN,nodeDN,oldBest);
   if(nodePN==0 || nodeDN==0 || nodePN>=phi || nodeDN>=delta) return;
   int sel=-1; uint32_t best1=INF, best2=INF;
   for(int i=0;i<(int)moves.size();i++){
     uint32_t val=isOR?moves[i].pn:moves[i].dn;
     if(val<best1){best2=best1;best1=val;sel=i;} else if(val<best2){best2=val;}
   }
   if(sel<0) return;
   Move m=moves[sel];
   uint32_t cphi,cdelta;
   if(isOR){
      cphi=min(phi,plusOne(best2));
      cdelta = satAdd(delta, m.dn); // subtract node dn with saturation carefully
      cdelta = (delta>=nodeDN ? delta-nodeDN : 1);
      cdelta = satAdd(cdelta,m.dn);
   } else {
      cphi=(phi>=nodePN ? phi-nodePN : 1);
      cphi=satAdd(cphi,m.pn);
      cdelta=min(delta,plusOne(best2));
   }
   tt->set(x,o,nodePN,nodeDN,m.col);
   // Save parent state, apply move, recurse.
   ht[m.col]++;
   dfpn(m.x,m.o,ht,ply+1,max(1u,cphi),max(1u,cdelta));
   ht[m.col]--;
   if((nodes&0x3fffff)==0){
     double sec=chrono::duration<double>(chrono::steady_clock::now()-startTime).count();
     cerr<<"PROGRESS nodes="<<nodes<<" sec="<<sec<<" tt_used="<<tt->used<<" root=";
     Entry *re=tt->find(rootX,rootO); if(re)cerr<<re->pn<<","<<re->dn<<" best="<<(int)re->best; cerr<<"\n";
   }
   if(NODE_LIMIT && nodes>=NODE_LIMIT) return;
   if(timeLimit>0 && (nodes%200000==0) && chrono::duration<double>(chrono::steady_clock::now()-startTime).count()>timeLimit) return;
 }
}
int main(int argc,char**argv){
 if(argc>1)W=atoi(argv[1]); if(argc>2)H=atoi(argv[2]);if(argc>3)K=atoi(argv[3]);if(argc>4)NODE_LIMIT=strtoull(argv[4],nullptr,10);if(argc>5)timeLimit=atof(argv[5]); int pow=20;if(argc>6)pow=atoi(argv[6]); if(argc>8)TARGET=atoi(argv[8]);
 if(W<1||H<1||K<1||W*H>63||W>15||H>10||K>max(W,H)||(TARGET!=1&&TARGET!=2)){cerr<<"unsupported dims\n";return 2;}
 startTime=chrono::steady_clock::now(); tt=new TT(pow); array<int,16> ht{};
 if(argc>7 && atoi(argv[7])>=0){int fc=atoi(argv[7]);if(fc>=W){cerr<<"bad opening column\n";return 2;} rootX=1ull<<(fc*H);ht[fc]=1;rootPly=1;}
 // root threshold INF, INF. Iterate until root is proved/disproved or limit.
 while(true){
   dfpn(rootX,rootO,ht,rootPly,INF,INF);
   Entry *r=tt->find(rootX,rootO); if(r) cerr<<"ROOT pn="<<r->pn<<" dn="<<r->dn<<" best="<<(int)r->best<<" nodes="<<nodes<<" sec="<<chrono::duration<double>(chrono::steady_clock::now()-startTime).count()<<" used="<<tt->used<<" hits="<<tt->hits<<"\n";
   if(r&&(r->pn==0||r->dn==0)){ cout<<"SOLVED target="<<TARGET<<" "<<(r->pn==0?"CAN_FORCE_WIN":"CANNOT_FORCE_WIN")<<"\n";break;}
   if((NODE_LIMIT&&nodes>=NODE_LIMIT)||(timeLimit>0&&chrono::duration<double>(chrono::steady_clock::now()-startTime).count()>timeLimit)){cout<<"UNSOLVED_LIMIT\n";break;}
   if(!r){cout<<"NO_ROOT\n";break;}
 }
}
