set datafile separator ","; set datafile missing "nan"
truth_file="Wetland_assimilation_MCMC/outputs/paper_plot_inputs_mcmc/truth_normalized_mcmc.csv"
mcmc_file="Wetland_assimilation_MCMC/outputs/paper_plot_inputs_mcmc/mcmc_normalized.csv"
rean_file="Wetland_assimilation_MCMC/outputs/paper_plot_inputs_mcmc/reanalysis_normalized_mcmc.csv"
outfile="paper_wetland_stage_three_views_mcmc.png"
set terminal pngcairo size 1600,1200 enhanced font "Helvetica,18"; set output outfile
epoch(d)=(d-25569.0)*86400.0
set xdata time; set timefmt "%s"; set format x "%Y-%m-%d"
load "plot_windows_wetland_mcmc.gp"
set multiplot layout 3,2
set grid xtics ytics lc rgb "#cccccc" lw 0.8; set tics nomirror; set border 3
ct="#d62728"; cm="#1f77b4"; cr="#2ca02c"; lw=2.2; ps=0.6; sub=3
set key top right inside box opaque font "Helvetica,15"; set ylabel "Inlet stage (m)"; set title "Early window"
plot [x0:x1] mcmc_file using (epoch($1)):4 w l lw lw lc rgb cm title "MCMC operational", rean_file using (epoch($1)):4 w l lw lw lc rgb cr title "MCMC reanalysis", truth_file every sub using (epoch($1)):4 w p pt 7 ps ps lc rgb ct title "Truth"
unset key; unset ylabel; set title "Late window"
plot [x2:x3] mcmc_file using (epoch($1)):4 w l lw lw lc rgb cm notitle, rean_file using (epoch($1)):4 w l lw lw lc rgb cr notitle, truth_file every sub using (epoch($1)):4 w p pt 7 ps ps lc rgb ct notitle
set ylabel "Outlet stage (m)"; unset title
plot [x0:x1] mcmc_file using (epoch($1)):6 w l lw lw lc rgb cm notitle, rean_file using (epoch($1)):6 w l lw lw lc rgb cr notitle, truth_file every sub using (epoch($1)):6 w p pt 7 ps ps lc rgb ct notitle
unset ylabel
plot [x2:x3] mcmc_file using (epoch($1)):6 w l lw lw lc rgb cm notitle, rean_file using (epoch($1)):6 w l lw lw lc rgb cr notitle, truth_file every sub using (epoch($1)):6 w p pt 7 ps ps lc rgb ct notitle
set ylabel "Outflow (m^3/day)"; set xlabel "Simulated date"; set xtics rotate by -30
plot [x0:x1] mcmc_file using (epoch($1)):7 w l lw lw lc rgb cm notitle, rean_file using (epoch($1)):7 w l lw lw lc rgb cr notitle, truth_file every sub using (epoch($1)):7 w p pt 7 ps ps lc rgb ct notitle
unset ylabel; set xlabel "Simulated date"
plot [x2:x3] mcmc_file using (epoch($1)):7 w l lw lw lc rgb cm notitle, rean_file using (epoch($1)):7 w l lw lw lc rgb cr notitle, truth_file every sub using (epoch($1)):7 w p pt 7 ps ps lc rgb ct notitle
unset multiplot; unset output
