// dot_equiv.cpp -- prove the direct renderDot writes BIT-IDENTICAL vertices to
// the old renderGlowLine -> segment() -> tri() -> pushVert() route.
//
// Both implementations are transcribed verbatim from text_c3d.cpp. The claim
// under test is that the affine reduces to the identity for a dot, so the
// scenic route and the direct write cannot differ by even one ULP.
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>

struct Vertex { float pos[3]; float color[4]; float uv[2]; };

// ---- globals, as in text_c3d.cpp -------------------------------------------
static float g_x, g_y, g_sx, g_sy, g_cosR, g_sinR, g_centerX;
static float g_r, g_g, g_b, g_a;
static float g_capExt = 0.3f, g_thickBoost = 1.7f;
static Vertex g_buf[64]; static int g_count;

static inline void affine(float px, float py, float& ox, float& oy) {
    const float cx = (px + g_centerX) * g_sx;
    const float cy = py * g_sy;
    ox = cx * g_cosR - cy * g_sinR + g_x;
    oy = cx * g_sinR + cy * g_cosR + g_y;
}
static inline void pushVert(float px, float py, float a) {
    if (g_count >= 64) return;
    Vertex& v = g_buf[g_count++];
    float ox, oy; affine(px, py, ox, oy);
    v.pos[0] = ox; v.pos[1] = oy; v.pos[2] = 0.0f;
    v.color[0] = g_r; v.color[1] = g_g; v.color[2] = g_b; v.color[3] = a;
    v.uv[0] = 0.0f; v.uv[1] = 0.0f;
}
static inline void tri(float ax,float ay,float aa,float bx,float by,float ba,
                       float cx,float cy,float ca){
    pushVert(ax,ay,aa); pushVert(bx,by,ba); pushVert(cx,cy,ca);
}
static void segment(float x1,float y1,float x2,float y2,float thickness){
    thickness *= g_thickBoost;
    float dx = x2-x1, dy = y2-y1;
    float len = std::sqrt(dx*dx + dy*dy);
    if (len < 0.001f) {
        const float s = thickness * 0.5f;
        tri(x1,y1,g_a, x1-s,y1,0.0f, x1,y1+s,0.0f);
        tri(x1,y1,g_a, x1,y1+s,0.0f, x1+s,y1,0.0f);
        tri(x1,y1,g_a, x1+s,y1,0.0f, x1,y1-s,0.0f);
        tri(x1,y1,g_a, x1,y1-s,0.0f, x1-s,y1,0.0f);
        return;
    }
    // (long-segment branch omitted -- unreachable for a dot)
}

// ---- OLD: renderDot -> renderGlowLine -> segment ---------------------------
static void renderDot_old(float x,float y,float size,float r,float g,float b,float a){
    g_x=0.0f; g_y=0.0f; g_sx=1.0f; g_sy=1.0f;
    g_cosR=1.0f; g_sinR=0.0f; g_centerX=0.0f;
    g_r=r; g_g=g; g_b=b; g_a=a;
    g_capExt=size; g_thickBoost=1.0f;
    segment(x,y,x,y,size);
    g_capExt=0.3f; g_thickBoost=1.7f;
}

// ---- NEW: the direct path, transcribed from the patched text_c3d.cpp -------
static void renderDot_new(float x,float y,float size,float r,float g,float b,float a){
    if (g_count + 12 > 64) return;
    const float s = size * 0.5f;
    const float rx[4] = { x - s, x,     x + s, x     };
    const float ry[4] = { y,     y + s, y,     y - s };
    for (int t = 0; t < 4; ++t) {
        const int i0 = t, i1 = (t + 1) & 3;
        const float vx[3] = { x, rx[i0], rx[i1] };
        const float vy[3] = { y, ry[i0], ry[i1] };
        const float va[3] = { a, 0.0f,   0.0f   };
        for (int k = 0; k < 3; ++k) {
            Vertex& v = g_buf[g_count++];
            v.pos[0]=vx[k]; v.pos[1]=vy[k]; v.pos[2]=0.0f;
            v.color[0]=r; v.color[1]=g; v.color[2]=b; v.color[3]=va[k];
            v.uv[0]=0.0f; v.uv[1]=0.0f;
        }
    }
}

int main(){
    // Values spanning what popup_fx actually produces: UI space is [0,1.333]
    // x [0,1] y, sizes from the popup's dot budget, plus adversarial cases.
    const float xs[]={0.0f,0.001f,0.5f,0.6667f,1.3333f,-0.25f,12.5f};
    const float ys[]={0.0f,0.002f,0.5f,0.925f,1.0f,-0.1f,7.75f};
    const float ss[]={0.0f,1e-7f,0.0015f,0.004f,0.02f,0.15f,3.0f};
    const float cs[]={0.0f,0.15f,0.5f,0.95f,1.0f};
    long cases=0, bad=0;
    for(float x:xs) for(float y:ys) for(float s:ss) for(float c:cs){
        Vertex oldv[12], newv[12];
        g_count=0; renderDot_old(x,y,s,c,1.0f-c,0.5f*c,c);
        int no=g_count; std::memcpy(oldv,g_buf,sizeof(oldv));
        g_count=0; renderDot_new(x,y,s,c,1.0f-c,0.5f*c,c);
        int nn=g_count; std::memcpy(newv,g_buf,sizeof(newv));
        ++cases;
        if(no!=nn || std::memcmp(oldv,newv,sizeof(oldv))!=0){
            ++bad;
            if(bad<=3){
                std::printf("MISMATCH x=%g y=%g size=%g c=%g  (old %d verts, new %d)\n",x,y,s,c,no,nn);
                for(int i=0;i<12;i++){
                    const uint32_t*a=(const uint32_t*)&oldv[i]; const uint32_t*b=(const uint32_t*)&newv[i];
                    for(int w=0;w<9;w++) if(a[w]!=b[w])
                        std::printf("   v%-2d word%d  old=0x%08x new=0x%08x\n",i,w,a[w],b[w]);
                }
            }
        }
    }
    std::printf("\n%ld cases, %ld mismatches -- %s\n", cases, bad,
                bad==0 ? "BIT-IDENTICAL" : "*** DIFFERS ***");
    return bad!=0;
}
