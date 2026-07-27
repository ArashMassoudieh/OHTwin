set datafile separator ","
set datafile missing "nan"
infile="Wetland_assimilation_MCMC/outputs/paper_plot_inputs_mcmc/mcmc_sigma_parameters_wetland.csv"
outfile="paper_sigma_estimates_mcmc_wetland.png"
set terminal pngcairo size 1200,700 enhanced font "Helvetica,18"
set output outfile

epoch(d)=(d-25569.0)*86400.0
central(meanv,medv) = (medv==medv ? medv : meanv)
pos(v) = (v==v && v>0 ? v : 1/0)
poscentral(meanv,medv) = pos(central(meanv,medv))

set xdata time
set timefmt "%s"
set format x "%Y-%m"
set xtics rotate by -30
set grid xtics ytics lc rgb "#cccccc" lw 0.8
set tics nomirror
set border 3
set logscale y
set format y "10^{%T}"
set xlabel "Simulated date"
set ylabel "Estimated {/Symbol s}_j"
set style fill transparent solid 0.25 noborder
set key top right inside box opaque font "Helvetica,15"
set title "MCMC posterior estimates of observation-noise standard deviations"

# Stage: mean,p025,p50,p975 at cols 4-7; Outflow at 8-11.
# Blank provisional-cycle intervals are ignored; central lines fall back to mean.
plot infile using (epoch($2)):(pos($5)):(pos($7)) with filledcurves lc rgb "#9ecae1" title "Stage 95% CI", \
     infile using (epoch($2)):(poscentral($4,$6)) with lines lw 2.2 lc rgb "#1f77b4" title "Stage median / point estimate", \
     infile using (epoch($2)):(pos($9)):(pos($11)) with filledcurves lc rgb "#fcae91" title "Outflow 95% CI", \
     infile using (epoch($2)):(poscentral($8,$10)) with lines lw 2.2 lc rgb "#d62728" title "Outflow median / point estimate"
unset output
