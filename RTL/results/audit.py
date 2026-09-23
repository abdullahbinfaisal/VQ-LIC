import sys, csv; sys.path.insert(0,'.')
from svc_model import *

EXPT = [("CTRL-A","16-48-64",[16,48,64]),
        ("CTRL-B","16-32-32-32-64-64",[16,32,32,32,64,64]),
        ("OOS-A","16-16-16-16-32-64",[16,16,16,16,32,64]),
        ("OOS-B","16-16-48-32-64",[16,16,48,32,64]),
        ("OOS-C","16-16-64-64",[16,16,64,64]),
        ("OOS-D","16-32-48-64-32-64",[16,32,48,64,32,64])]
# Every distinct topology recorded anywhere in the repo (docs/roofline/logs).
REPO = [("repo","8-32-64-64-64-64",[8,32,64,64,64,64]),
        ("repo","12-32-64-64-64-64",[12,32,64,64,64,64]),
        ("repo","16-32-64-64-64-64",[16,32,64,64,64,64]),
        ("repo","16-32-64-32-64-64",[16,32,64,32,64,64]),
        ("repo","8-32-32-32-64-64",[8,32,32,32,64,64]),
        ("repo","16-32-32-64-64-64",[16,32,32,64,64,64]),
        ("repo","8-48-64-64-64-64",[8,48,64,64,64,64]),
        ("repo","8-24-64-64-64-64",[8,24,64,64,64,64]),
        ("repo","8-16-64-96-64-64",[8,16,64,96,64,64]),
        ("audit","16-64-64-64 (withdrawn)",[16,64,64,64])]

rows=[]; print("%-8s %-26s %2s %4s %5s %4s %4s %5s %7s  %-9s %s"%(
      "set","candidate","b","H","W","G","cin","cout","G*cin","verdict","first violated limit"))
print("-"*112)
for tag,name,sched in EXPT+REPO:
    blocks=list(walk(sched)); infeas=[]
    for b in blocks:
        bad=[(n,v,l) for n,v,l in check(b) if v>l]
        rows.append(dict(set=tag,candidate=name,block=b['block'],H=b['H'],W=b['W'],
            G=b['G'],stride=b['stride'],cin=b['cin'],cout=b['cout'],
            G_x_cin=b['cg'],MAX_CG_PRODUCT=CAP['MAX_CG_PRODUCT'],
            feasible=("no" if bad else "yes"),
            reason=(bad[0][0]+" : %d > %d"%(bad[0][1],bad[0][2]) if bad else "")))
        if bad: infeas.append((b['block'],bad[0]))
        print("%-8s %-26s %2d %4d %5d %4d %4d %5d %7d  %-9s %s"%(
              tag,name if b['block']==1 else "",b['block'],b['H'],b['W'],b['G'],
              b['cin'],b['cout'],b['cg'],
              "FAIL" if bad else "ok",
              (bad[0][0]+" : %d > %d"%(bad[0][1],bad[0][2])) if bad else ""))
    print("%-8s %-26s => %s"%("", "", "INFEASIBLE at block %d"%infeas[0][0] if infeas else "FEASIBLE"))
with open("hw_feasibility_audit.csv","w",newline="") as f:
    w=csv.DictWriter(f,fieldnames=list(rows[0].keys())); w.writeheader(); w.writerows(rows)
print("\nwrote hw_feasibility_audit.csv (%d block rows)"%len(rows))
