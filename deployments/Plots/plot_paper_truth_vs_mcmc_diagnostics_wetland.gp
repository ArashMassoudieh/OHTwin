set datafile separator ","; set datafile missing "nan"
truth_file="Wetland_assimilation_MCMC/outputs/paper_plot_inputs_mcmc/truth_normalized_mcmc.csv"
mcmc_file="Wetland_assimilation_MCMC/outputs/paper_plot_inputs_mcmc/mcmc_normalized.csv"
outfile="paper_truth_vs_mcmc_diagnostics_wetland.png"
set terminal pngcairo size 1600,1000 enhanced font "Helvetica,18"; set output outfile
epoch(d)=(d-25569.0)*86400.0
set xdata time; set timefmt "%s"; set format x "%Y-%m-%d"
x0=epoch(43833.0); x1=epoch(43843.0); x2=epoch(44555.0); x3=epoch(44565.0)
set multiplot layout 2,2
set grid xtics ytics lc rgb "#cccccc" lw 0.8; set tics nomirror; set border 3
ct="#d62728"; cm="#1f77b4"; lw=2.2; ps=0.6; sub=3
set key top right inside box opaque font "Helvetica,15"; set ylabel "Mid-stage (m)"; set title "Early window"
plot [x0:x1] mcmc_file using (epoch($1)):5 w l lw lw lc rgb cm title "MCMC assimilation", truth_file every sub using (epoch($1)):5 w p pt 7 ps ps lc rgb ct title "Truth"
unset key; unset ylabel; set title "Late window"
plot [x2:x3] mcmc_file using (epoch($1)):5 w l lw lw lc rgb cm notitle, truth_file every sub using (epoch($1)):5 w p pt 7 ps ps lc rgb ct notitle
set ylabel "HRT (day)"; set xlabel "Simulated date"; set xtics rotate by -30; unset title
plot [x0:x1] mcmc_file using (epoch($1)):10 w l lw lw lc rgb cm notitle, truth_file every sub using (epoch($1)):10 w p pt 7 ps ps lc rgb ct notitle
unset ylabel; set xlabel "Simulated date"
plot [x2:x3] mcmc_file using (epoch($1)):10 w l lw lw lc rgb cm notitle, truth_file every sub using (epoch($1)):10 w p pt 7 ps ps lc rgb ct notitle
unset multiplot; unset output
