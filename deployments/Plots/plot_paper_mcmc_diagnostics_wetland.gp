set datafile separator ","
set datafile missing "nan"
infile="Wetland_assimilation_MCMC/outputs/paper_plot_inputs_mcmc/mcmc_diagnostics_wetland.csv"
outfile="paper_mcmc_diagnostics_wetland.png"
set terminal pngcairo size 1100,1500 enhanced font "Helvetica,18"
set output outfile
epoch(d)=(d-25569.0)*86400.0
set xdata time; set timefmt "%s"; set format x "%Y-%m"
set multiplot layout 4,1
set grid xtics ytics lc rgb "#cccccc" lw 0.8; set tics nomirror; set border 3
set format x ""; unset xlabel
set ylabel "ESS"; set title "(a) Effective sample size"
plot infile using (epoch($2)):4 with linespoints lw 2 pt 7 ps 0.8 lc rgb "#1f77b4" notitle
set ylabel "Pool size"; set title "(b) Retained posterior samples"
plot infile using (epoch($2)):5 with linespoints lw 2 pt 7 ps 0.8 lc rgb "#2ca02c" notitle
set ylabel "Fraction (-)"; set yrange [0:1]; set title "(c) Sampling diagnostics"
set key bottom right inside box opaque font "Helvetica,15"
plot infile using (epoch($2)):6 with lines lw 2 lc rgb "#9467bd" title "Plateaued chains", \
     infile using (epoch($2)):7 with lines lw 2 lc rgb "#d62728" title "Acceptance rate", \
     0.5 with lines dt 2 lc rgb "#555555" lw 1.5 title "Quorum = 0.5"
unset key; set yrange [*:*]
set format x "%Y-%m"; set xtics rotate by -30; set xlabel "Simulated date" offset 0,-0.7
set ylabel "Sweeps"; set y2label "Evaluations"; set y2tics; set title "(d) Computational effort"
plot infile using (epoch($2)):8 axes x1y1 with lines lw 2 lc rgb "#1f77b4" title "Sweeps", \
     infile using (epoch($2)):9 axes x1y2 with lines lw 2 lc rgb "#ff7f0e" title "Evaluations"
unset multiplot; unset output
