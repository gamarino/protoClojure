# Medians of run-stages.sh output files: one row per stage, one column per
# runtime label, each cell "median-ms/runs".
#
#   python3 median.py FILE...
import collections,statistics,sys
d=collections.defaultdict(list); rts=[]; st=[]
for f in sys.argv[1:]:
    for l in open(f):
        p=l.split(); d[(p[0],p[1])].append(int(p[2]))
        if p[0] not in rts: rts.append(p[0])
        if p[1] not in st: st.append(p[1])
print('stage'.ljust(12)+''.join(r.rjust(9) for r in rts))
for s in st: print(s.ljust(12)+''.join((str(int(statistics.median(d[(r,s)])))+'/'+str(len(d[(r,s)]))).rjust(9) if (r,s) in d else ''.rjust(9) for r in rts))
