set datafile separator ","
set datafile missing "nan"
infile="Wetland_assimilation_MCMC/outputs/paper_plot_inputs_mcmc/mcmc_physical_parameters_wetland.csv"
outfile="paper_physical_parameters_mcmc_wetland.png"
ksat_truth_file="Wetland_truth/drift/ksat_drift_wetland.csv"

set terminal pngcairo size 1600,1250 enhanced font "Helvetica,18"
set output outfile

epoch(d)=(d-25569.0)*86400.0
# For provisional MCMC cycles p025/p975 may be blank. Use p50 when present,
# otherwise fall back to mean/point estimate. Positive-only variant is used
# on logarithmic axes.
central(meanv,medv) = (medv==medv ? medv : meanv)
pos(v) = (v==v && v>0 ? v : 1/0)
poscentral(meanv,medv) = pos(central(meanv,medv))

set xdata time
set timefmt "%s"
true_runoff=0.75
true_outlet_alpha=100000.0
true_pond=1.0
true_evap=1.0

set multiplot layout 3,2
set grid xtics ytics lc rgb "#cccccc" lw 0.8
set tics nomirror
set border 3
cband="#9ecae1"
cmed="#1f77b4"
ctruth="#d62728"
set style fill transparent solid 0.35 noborder
set format x ""
unset xlabel
unset key

# CSV layout: metadata columns 1-3, then mean,p025,p50,p975 per parameter.
set ylabel "Runoff coefficient (-)"
set yrange [0:1]
set title "(a) Catchment runoff coefficient"
plot infile using (epoch($2)):5:7 with filledcurves lc rgb cband notitle, \
     true_runoff with lines dt 2 lc rgb ctruth lw 2 notitle, \
     infile using (epoch($2)):(central($4,$6)) with lines lw 2.2 lc rgb cmed notitle

set logscale y
set format y "10^{%T}"
set autoscale y
set ylabel "Outlet {/Symbol a} (m^2/day)"
set title "(b) Wetland outlet alpha"
plot infile using (epoch($2)):(pos($9)):(pos($11)) with filledcurves lc rgb cband notitle, \
     true_outlet_alpha with lines dt 2 lc rgb ctruth lw 2 notitle, \
     infile using (epoch($2)):(poscentral($8,$10)) with lines lw 2.2 lc rgb cmed notitle

unset logscale y
set format y "%g"
set ylabel "Pond {/Symbol a} multiplier (-)"
set yrange [0:*]
set title "(c) Pond alpha multiplier"
plot infile using (epoch($2)):13:15 with filledcurves lc rgb cband notitle, \
     true_pond with lines dt 2 lc rgb ctruth lw 2 notitle, \
     infile using (epoch($2)):(central($12,$14)) with lines lw 2.2 lc rgb cmed notitle

set ylabel "Evaporation coefficient (-)"
set autoscale y
set title "(d) Evaporation coefficient"
plot infile using (epoch($2)):17:19 with filledcurves lc rgb cband notitle, \
     true_evap with lines dt 2 lc rgb ctruth lw 2 notitle, \
     infile using (epoch($2)):(central($16,$18)) with lines lw 2.2 lc rgb cmed notitle

set format x "%Y-%m"
set xtics rotate by -30
set xlabel "Simulated date" offset 0,-0.7
set logscale y
set format y "10^{%T}"
set autoscale y
set ylabel "Soil K_{sat} (m/day)"
set title "(e) Soil hydraulic conductivity"
set key top right inside box opaque font "Helvetica,15"
plot infile using (epoch($2)):(pos($21)):(pos($23)) with filledcurves lc rgb cband title "95% credible interval", \
     ksat_truth_file using (epoch($1)):(pos($2)) with lines dt 2 lw 2 lc rgb ctruth title "Synthetic truth", \
     infile using (epoch($2)):(poscentral($20,$22)) with lines lw 2.2 lc rgb cmed title "Posterior median / point estimate"

unset logscale y
set format y "%g"
unset border
unset xtics
unset ytics
unset xlabel
unset ylabel
unset title
unset key
plot [0:1][0:1] NaN notitle

unset multiplot
unset output
