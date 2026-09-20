// fan_equiv.cpp -- prove the INDEXED 6-vertex fan replays the EXACT triangles
// the old unindexed expansion wrote, for both producers in text_c3d.cpp:
//   * segment()'s long path : 15 vertices -> 6 + 15 indices
//   * the dot (degenerate)  : 12 vertices -> 6 + 15 indices (last tri collapses)
// Both transcribed verbatim from the file, before and after.
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdint>

struct V { float x, y, a; };
static bool same(const V& p, const V& q) {   // BIT-exact, not epsilon
    return std::memcmp(&p, &q, sizeof(V)) == 0;
}

// ---- OLD: independent triangles ------------------------------------------
static int oldSegment(V* o, float x1,float y1,float x2,float y2,float th,
                      float a,float capExt,float boost) {
    th *= boost;
    float dx=x2-x1, dy=y2-y1, len=std::sqrt(dx*dx+dy*dy);
    int n=0;
    auto tri=[&](V p,V q,V r){ o[n++]=p; o[n++]=q; o[n++]=r; };
    if (len < 0.001f) {
        const float s = th*0.5f;
        tri({x1,y1,a},{x1-s,y1,0},{x1,y1+s,0});
        tri({x1,y1,a},{x1,y1+s,0},{x1+s,y1,0});
        tri({x1,y1,a},{x1+s,y1,0},{x1,y1-s,0});
        tri({x1,y1,a},{x1,y1-s,0},{x1-s,y1,0});
        return n;
    }
    const float ndx=dx/len, ndy=dy/len;
    const float px=-ndy*th, py=ndx*th;
    const float ex=ndx*capExt, ey=ndy*capExt;
    const float v1x=x1-ex-px, v1y=y1-ey-py;
    const float v2x=x1-ex+px, v2y=y1-ey+py;
    const float v3x=x2+ex+px, v3y=y2+ey+py;
    const float v5x=x2+ex-px, v5y=y2+ey-py;
    tri({x1,y1,a},{v1x,v1y,0},{v2x,v2y,0});
    tri({x1,y1,a},{v2x,v2y,0},{v3x,v3y,0});
    tri({x1,y1,a},{v3x,v3y,0},{x2,y2,a});
    tri({x1,y1,a},{x2,y2,a},{v5x,v5y,0});
    tri({x1,y1,a},{v5x,v5y,0},{v1x,v1y,0});
    return n;
}

// ---- NEW: 6-vertex slot, replayed through the fan index pattern -----------
static int newSegmentVerts(V* o, float x1,float y1,float x2,float y2,float th,
                           float a,float capExt,float boost) {
    th *= boost;
    float dx=x2-x1, dy=y2-y1, len=std::sqrt(dx*dx+dy*dy);
    if (len < 0.001f) {
        const float s = th*0.5f;
        const float rx[5]={x1-s,x1,x1+s,x1,x1-s};
        const float ry[5]={y1,y1+s,y1,y1-s,y1};
        o[0]={x1,y1,a};
        for(int i=0;i<5;i++) o[1+i]={rx[i],ry[i],0.0f};
        return 6;
    }
    const float ndx=dx/len, ndy=dy/len;
    const float px=-ndy*th, py=ndx*th;
    const float ex=ndx*capExt, ey=ndy*capExt;
    const float rx[5]={x1-ex-px, x1-ex+px, x2+ex+px, x2,   x2+ex-px};
    const float ry[5]={y1-ey-py, y1-ey+py, y2+ey+py, y2,   y2+ey-py};
    const float ra[5]={0.0f,0.0f,0.0f,a,0.0f};
    o[0]={x1,y1,a};
    for(int i=0;i<5;i++) o[1+i]={rx[i],ry[i],ra[i]};
    return 6;
}
// the IBO pattern built in text_c3d::init
static void fanIndices(unsigned short* ix){
    for(int t=0;t<5;t++){ ix[t*3+0]=0; ix[t*3+1]=(unsigned short)(1+t); ix[t*3+2]=(unsigned short)(1+((t+1)%5)); }
}

int main(){
    unsigned short ix[15]; fanIndices(ix);
    const float xs[]={0.0f,0.001f,0.5f,-0.25f,1.3333f,12.5f};
    const float ys[]={0.0f,0.002f,0.5f,-0.1f,1.0f,7.75f};
    const float ths[]={0.02f,0.1f,0.35f,1.7f};
    const float as[]={0.0f,0.25f,1.0f};
    long cases=0, bad=0, degen=0;
    for(float x1:xs) for(float y1:ys) for(float x2:xs) for(float y2:ys)
    for(float th:ths) for(float a:as) {
        V o[15], nv[6];
        int no = oldSegment(o, x1,y1,x2,y2,th,a,0.3f,1.7f);
        newSegmentVerts(nv, x1,y1,x2,y2,th,a,0.3f,1.7f);
        const bool isDot = (no==12);
        if (isDot) ++degen;
        ++cases;
        // replay the new vertices through the indices and compare triangles
        int cmp = isDot ? 12 : 15;    // a dot's 5th triangle is the added degenerate one
        for (int k=0;k<cmp;k++){
            if(!same(o[k], nv[ix[k]])){ if(++bad<=3)
                std::printf("MISMATCH seg(%g,%g -> %g,%g th=%g a=%g) vert %d\n",x1,y1,x2,y2,th,a,k);
                break; }
        }
        if (isDot) {   // the extra triangle MUST be degenerate (zero area)
            const V&A=nv[ix[12]];const V&B=nv[ix[13]];const V&C=nv[ix[14]];
            const float ar=std::fabs((B.x-A.x)*(C.y-A.y)-(C.x-A.x)*(B.y-A.y));
            if(ar!=0.0f){ ++bad; std::printf("dot pad triangle NOT degenerate: area %g\n",ar); }
        }
    }
    std::printf("\n%ld cases (%ld degenerate/dot), %ld mismatches -- %s\n",
                cases, degen, bad, bad==0 ? "TRIANGLE-EXACT" : "*** DIFFERS ***");
    return bad!=0;
}
