# =============================================================================
# plot_paper_wetland_stage_three_views.gp
# Three wetland stage/output views using truth, operational assimilation, and
# reanalysis/final parameters.
# =============================================================================

set datafile separator ","
set datafile missing "nan"

truth_file      = "Wetland_truth/outputs/selected_output.csv"
assim_file      = "Wetland_assimilation/outputs/selected_output.csv"
reanalysis_file = "Wetland_assimilation/outputs/reanalysis_output.csv"
outfile         = "paper_wetland_stage_three_views.png"

set terminal pngcairo size 1600,1200 enhanced font "Helvetica,18"
set output outfile

epoch(d) = (d - 25569.0) * 86400.0
set xdata time
set timefmt "%s"
set format x "%Y-%m-%d"

x0 = epoch(43833.0)
x1 = epoch(43843.0)
x2 = epoch(44555.0)
x3 = epoch(44565.0)

set multiplot layout 3,2
set grid xtics ytics lc rgb "#cccccc" lw 0.8
set tics nomirror
set border 3

c_truth = "#d62728"
c_assim = "#1f77b4"
c_reanalysis = "#2ca02c"
lw_curve = 2.2
ps_truth = 0.6
sub_every = 3

set key top right inside box opaque samplen 2 spacing 1.2 font "Helvetica,16" width 0
set ylabel "Inlet stage (m)" offset 1,0
set title "Early window" font "Helvetica,18"
plot [x0:x1] assim_file using (epoch($5)):6 with lines lc rgb c_assim lw lw_curve title "Assimilation", \
             reanalysis_file using (epoch($5)):6 with lines lc rgb c_reanalysis lw lw_curve title "Reanalysis", \
             truth_file every sub_every using (epoch($5)):6 with points lc rgb c_truth pt 7 ps ps_truth title "Truth"
unset key
unset ylabel
set title "Late window" font "Helvetica,18"
plot [x2:x3] assim_file using (epoch($5)):6 with lines lc rgb c_assim lw lw_curve notitle, \
             reanalysis_file using (epoch($5)):6 with lines lc rgb c_reanalysis lw lw_curve notitle, \
             truth_file every sub_every using (epoch($5)):6 with points lc rgb c_truth pt 7 ps ps_truth notitle

set ylabel "Outlet stage (m)" offset 1,0
unset title
plot [x0:x1] assim_file using (epoch($9)):10 with lines lc rgb c_assim lw lw_curve notitle, \
             reanalysis_file using (epoch($9)):10 with lines lc rgb c_reanalysis lw lw_curve notitle, \
             truth_file every sub_every using (epoch($9)):10 with points lc rgb c_truth pt 7 ps ps_truth notitle
unset ylabel
plot [x2:x3] assim_file using (epoch($9)):10 with lines lc rgb c_assim lw lw_curve notitle, \
             reanalysis_file using (epoch($9)):10 with lines lc rgb c_reanalysis lw lw_curve notitle, \
             truth_file every sub_every using (epoch($9)):10 with points lc rgb c_truth pt 7 ps ps_truth notitle

set ylabel "Outflow (m^3/day)" offset 1,0
set xlabel "Simulated date" offset 0,-0.6
set xtics rotate by -30
plot [x0:x1] assim_file using (epoch($11)):12 with lines lc rgb c_assim lw lw_curve notitle, \
             reanalysis_file using (epoch($11)):12 with lines lc rgb c_reanalysis lw lw_curve notitle, \
             truth_file every sub_every using (epoch($11)):12 with points lc rgb c_truth pt 7 ps ps_truth notitle
unset ylabel
set xlabel "Simulated date" offset 0,-0.6
plot [x2:x3] assim_file using (epoch($11)):12 with lines lc rgb c_assim lw lw_curve notitle, \
             reanalysis_file using (epoch($11)):12 with lines lc rgb c_reanalysis lw lw_curve notitle, \
             truth_file every sub_every using (epoch($11)):12 with points lc rgb c_truth pt 7 ps ps_truth notitle

unset multiplot
unset output
