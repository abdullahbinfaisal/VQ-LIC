import sys,csv; sys.path.insert(0,'.')
from svc_model import *
EXPT=[("CTRL-A","16-48-64",[16,48,64]),("CTRL-B","16-32-32-32-64-64",[16,32,32,32,64,64]),
      ("OOS-A","16-16-16-16-32-64",[16,16,16,16,32,64]),("OOS-B","16-16-48-32-64",[16,16,48,32,64]),
      ("OOS-C","16-16-64-64",[16,16,64,64]),("OOS-D","16-32-48-64-32-64",[16,32,48,64,32,64])]
rows=[]
for tag,name,s in EXPT:
    b,cyc,ms=predict(s)
    print("\n%s  %s   T_svc = %d cyc = %.4f ms   binding = %s"%(
          tag,name,cyc,ms,"/".join(x['bind'] for x in b)))
    print("  blk cin cout   cyc/grp  ngrp_out     T_DW      T_PW    T_blk  bind")
    for x in b:
        ng=x['Ho']*x['Wo']//L
        print("  %3d %3d %4d %9d %9d %8d %9d %8d  %s"%(
              x['block'],x['cin'],x['cout'],pw_cyc_per_group(x['cin'],x['cout']),ng,
              x['t_dw'],x['t_pw'],x['t_blk'],x['bind']))
        rows.append(dict(candidate=name,tag=tag,block=x['block'],H_in=x['H'],W_in=x['W'],
          G=x['G'],stride=x['stride'],cin=x['cin'],cout=x['cout'],
          batches=max(1,ceil_div(x['cout'],Q)),Q_last=x['cout']-(max(1,ceil_div(x['cout'],Q))-1)*Q,
          pw_cyc_per_group=pw_cyc_per_group(x['cin'],x['cout']),ngroups_out=ng,
          T_DW_cyc=x['t_dw'],T_PW_cyc=x['t_pw'],T_block_cyc=x['t_blk'],
          binding=x['bind'],T_block_ms=round(x['t_blk']/F_CLK*1e3,6)))
with open("service_model_dump.csv","w",newline="") as f:
    w=csv.DictWriter(f,fieldnames=list(rows[0].keys())); w.writeheader(); w.writerows(rows)
print("\n--- totals ---")
for tag,name,s in EXPT:
    b,cyc,ms=predict(s); print("  %-8s %-22s %9d cyc  %8.4f ms  %s"%(tag,name,cyc,ms,"/".join(x['bind'] for x in b)))
print("\nwrote service_model_dump.csv")
