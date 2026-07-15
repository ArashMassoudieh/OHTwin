set datafile separator ","; set datafile missing "nan"
truth_file="Wetland_assimilation_MCMC/outputs/paper_plot_inputs_mcmc/truth_normalized_mcmc.csv"
mcmc_file="Wetland_assimilation_MCMC/outputs/paper_plot_inputs_mcmc/mcmc_normalized.csv"
outfile="paper_wetland_hrt_full_period_mcmc.png"
set terminal pngcairo size 1600,650 enhanced font "Helvetica,18"; set output outfile
epoch(d)=(d-25569.0)*86400.0
set xdata time; set timefmt "%s"; set format x "%Y-%m"; set xtics rotate by -30
set grid xtics ytics lc rgb "#cccccc" lw 0.8; set tics nomirror; set border 3
set title "Hydraulic residence time over the full simulation period"
set xlabel "Simulated date"; set ylabel "HRT (day)"
set key top right inside box opaque font "Helvetica,15"
plot mcmc_file using (epoch($1)):10 w l lw 2.2 lc rgb "#1f77b4" title "MCMC assimilation", \
     truth_file every 5 using (epoch($1)):10 w p pt 7 ps 0.45 lc rgb "#d62728" title "Truth"
unset output
