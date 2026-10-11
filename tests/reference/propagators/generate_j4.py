# Independent high-precision transcription of the pinned MIT Julia reference.
from pathlib import Path
import mpmath as m
m.mp.dps=60
R=m.mpf('6378137'); mu=m.mpf('3.986004415e14'); j2=m.mpf('0.0010826261738522227'); j4=m.mpf('-1.6198975999169731e-6')
a=m.mpf('7000000'); e=m.mpf('.01'); inc=m.radians(m.mpf('98.5')); O=m.radians(25); w=m.radians(70); M=m.radians(10)
p=(a/R)*(1-e**2); beta=m.sqrt(1-e**2); s=m.sin(inc); c=m.cos(inc); n=m.sqrt(mu/a**3)
nbar=n*(1+m.mpf(3)/4*j2/p**2*beta*(2-3*s**2)+m.mpf(3)/128*j2**2/p**4*beta*(120+64*beta-40*beta**2+(-240-192*beta+40*beta**2)*s**2+(105+144*beta+25*beta**2)*s**4)-m.mpf(45)/128*j4/p**4*beta*e**2*(-8+40*s**2-35*s**4))
node=-m.mpf(3)/2*nbar*j2/p**2*c+m.mpf(3)/32*nbar*j2**2/p**4*c*(-36-4*e**2+48*beta+(40-5*e**2-72*beta)*s**2)+m.mpf(15)/32*n*j4/p**4*c*(8+12*e**2-(14+21*e**2)*s**2)
apsis=m.mpf(3)/4*nbar*j2/p**2*(4-5*s**2)+m.mpf(3)/128*nbar*j2**2/p**4*(384+96*e**2-384*beta+(-824-116*e**2+1056*beta)*s**2+(430-5*e**2-720*beta)*s**4)-m.mpf(15)/16*n*j2**2/p**4*e**2*c**4-m.mpf(15)/128*n*j4/p**4*(64+72*e**2-(248+252*e**2)*s**2+(196+189*e**2)*s**4)
def position(t):
    mean=M+nbar*t
    E=m.findroot(lambda E:E-e*m.sin(E)-mean, mean)
    x=a*(m.cos(E)-e); y=a*beta*m.sin(E)
    om=O+node*t; ap=w+apsis*t
    X=m.cos(ap)*x-m.sin(ap)*y; Y=m.sin(ap)*x+m.cos(ap)*y
    return m.matrix([m.cos(om)*X-m.sin(om)*c*Y,m.sin(om)*X+m.cos(om)*c*Y,s*Y])
rows=[]
for t in [-2592000,-86400,0,86400,2592000]:
    r=position(m.mpf(t)); v=m.diff(position,m.mpf(t))
    rows.append(str(t)+' '+' '.join(m.nstr(x,22) for x in [*r,*v]))
Path('tests/reference/propagators/j4.txt').write_text('\n'.join(rows)+'\n',encoding='ascii',newline='\n')
