# PondeMate piece silhouette prototype (1bit) - draws 12 pieces, mask, and a preview board
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageChops
SQ=66; P=60; OFF=(SQ-P)//2; SS=8   # square, piece box, supersample
OUTLINE=3; HALO=2; DETAIL=3        # px at final size

def U(v): return v*P*SS/100.0       # 0..100 units -> supersampled px
def poly(d,pts,fill): d.polygon([(U(x),U(y)) for x,y in pts],fill=fill)
def circ(d,cx,cy,r,fill): d.ellipse([U(cx-r),U(cy-r),U(cx+r),U(cy+r)],fill=fill)
def rect(d,x0,y0,x1,y1,fill,rad=0): d.rounded_rectangle([U(x0),U(y0),U(x1),U(y1)],radius=U(rad),fill=fill)
def line(d,pts,w,fill): d.line([(U(x),U(y)) for x,y in pts],fill=fill,width=int(U(w)),joint="curve")

def base(d,f):
    rect(d,20,84,80,95,f,3)
    poly(d,[(30,84),(70,84),(66,76),(34,76)],f)

def shape(kind):
    """returns (silhouette L-img, details list of line specs (pts,width) in units)"""
    im=Image.new("L",(P*SS,P*SS),0); d=ImageDraw.Draw(im); F=255; det=[]
    if kind=="P":
        circ(d,50,28,13,F)
        rect(d,34,42,66,49,F,3)
        poly(d,[(41,46),(59,46),(67,78),(33,78)],F); base(d,F)
        det=[([(36,45.5),(64,45.5)],0)]  # none really
        det=[]
    elif kind=="R":
        poly(d,[(18,20),(30,20),(30,29),(43,29),(43,20),(57,20),(57,29),(70,29),(70,20),(82,20),(82,42),(18,42)],F)
        poly(d,[(26,42),(74,42),(72,76),(28,76)],F)
        rect(d,24,72,76,86,F,2); base(d,F)
        det=[([(21,44),(79,44)],DETAIL),([(26,72.5),(74,72.5)],DETAIL)]
    elif kind=="B":
        circ(d,50,10,6,F)
        d.ellipse([U(33),U(16),U(67),U(62)],fill=F)
        poly(d,[(50,13),(40,26),(60,26)],F)
        rect(d,34,58,66,65,F,3)
        poly(d,[(42,62),(58,62),(66,78),(34,78)],F); base(d,F)
        det=[([(45,30),(57,44)],DETAIL),([(37,61.5),(63,61.5)],DETAIL)]
    elif kind=="N":
        poly(d,[(30,78),(34,64),(44,54),(40,52),(28,54),(18,50),(15,43),(20,36),(30,26),(36,14),(42,22),(48,12),(54,18),(64,22),(74,34),(80,52),(78,66),(74,78)],F); base(d,F)
        det="N"
    elif kind=="Q":
        pts=[(24,58),(16,24),(33,42),(38,14),(50,38),(62,14),(67,42),(84,24),(76,58)]
        poly(d,pts,F)
        for x,y in [(16,22),(38,12),(50,30),(62,12),(84,22)]: circ(d,x,y,6,F)
        rect(d,26,56,74,63,F,3)
        poly(d,[(34,60),(66,60),(68,78),(32,78)],F); base(d,F)
        det=[([(29,59.5),(71,59.5)],DETAIL)]
    elif kind=="K":
        rect(d,45.5,1,54.5,19,F,1); rect(d,39,6,61,13,F,1)
        rect(d,42,17,58,23,F,2)
        poly(d,[(26,22),(74,22),(62,42),(38,42)],F)
        rect(d,30,40,70,47,F,3)
        poly(d,[(39,45),(61,45),(69,78),(31,78)],F); base(d,F)
        det=[([(30,24.5),(70,24.5)],DETAIL),([(33,43.5),(67,43.5)],DETAIL)]
    return im,det

def small(im): return im.resize((P,P),Image.BOX).point(lambda v:255 if v>=128 else 0)
def dil(b,r): return b.filter(ImageFilter.MaxFilter(2*r+1))
def ero(b,r): return b.filter(ImageFilter.MinFilter(2*r+1))

def piece(kind,white):
    """returns (ink, halo) as 1-bit-ish L images of size PxP: 255 = set"""
    sil,det=shape(kind)
    s=small(sil)
    detl=Image.new("L",(P*SS,P*SS),0); dd=ImageDraw.Draw(detl)
    if det=="N":
        circ(dd,34,33,3.2,255)                         # eye
        line(dd,[(44,20),(62,30),(70,46),(72,62)],DETAIL,255)  # mane line
        det=[]
    for pts,w in det:
        if w: line(dd,pts,w,255)
    dl=small(detl)
    if white:
        ring=ImageChops.subtract(s,ero(s,OUTLINE-1 if False else OUTLINE))
        ink=ImageChops.lighter(ring,ImageChops.multiply(dl,s))
    else:
        ink=ImageChops.subtract(s,dl)
    halo=dil(s,HALO)
    return ink,halo

def board(position,path,scale=1):
    W=SQ*8; img=Image.new("L",(W,W),255); px=img.load()
    for r in range(8):
        for f in range(8):
            if (r+f)%2==1:
                for y in range(SQ):
                    for x in range(SQ):
                        if (x+y)%2==0: px[f*SQ+x,r*SQ+y]=0
    cache={}
    for r,row in enumerate(position):
        for f,c in enumerate(row):
            if c==".": continue
            k=(c.upper(),c.isupper())
            if k not in cache: cache[k]=piece(*k)
            ink,halo=cache[k]
            ox,oy=f*SQ+OFF,r*SQ+OFF
            img.paste(255,(ox,oy),halo)
            img.paste(0,(ox,oy),ink)
    img=img.convert("1")
    if scale>1: img=img.resize((W*scale,W*scale),Image.NEAREST)
    img.save(path)

start=["rnbqkbnr","pppppppp","........","........","........","........","PPPPPPPP","RNBQKBNR"]
show=["rnbqkp..","..RNBQKP","KQRBNP..","..kqrbnp","........","........","........","........"]
board(start,"board_start_v2.png")
board(show,"board_show_x2_v2.png",2)
