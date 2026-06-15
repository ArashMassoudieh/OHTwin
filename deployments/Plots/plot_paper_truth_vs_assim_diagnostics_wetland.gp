# =============================================================================
# plot_paper_truth_vs_assim_diagnostics_wetland.gp
# Wetland diagnostic channels: mid-stage and HRT only.
# Uses normalized CSVs prepared by prepare_wetland_plot_inputs.py.
# =============================================================================

set datafile separator ","
set datafile missing "nan"

truth_file = "Wetland_assimilation/outputs/paper_plot_inputs/truth_normalized.csv"
assim_file = "Wetland_assimilation/outputs/paper_plot_inputs/assim_normalized.csv"
outfile    = "paper_truth_vs_assim_diagnostics_wetland.png"

set terminal pngcairo size 1600,1000 enhanced font "Helvetica,18"
set output outfile

epoch(d) = (d - 25569.0) * 86400.0
set xdata time
set timefmt "%s"
set format x "%Y-%m-%d"

x0 = epoch(43833.0)
x1 = epoch(43843.0)
x2 = epoch(44555.0)
x3 = epoch(44565.0)

set multiplot layout 2,2
set grid xtics ytics lc rgb "#cccccc" lw 0.8
set tics nomirror
set border 3

c_truth = "#d62728"
c_assim = "#1f77b4"
ps_truth = 0.6
lw_assim = 2.2
sub_every = 3

set key top right inside box opaque samplen 2 spacing 1.2 font "Helvetica,16" width 0
set ylabel "Mid-stage (m)" offset 1,0
set title "Early window" font "Helvetica,18"
plot [x0:x1] assim_file using (epoch($1)):5 with lines lc rgb c_assim lw lw_assim title "Assimilation model", \
             truth_file every sub_every using (epoch($1)):5 with points lc rgb c_truth pt 7 ps ps_truth title "Truth"

unset key
unset ylabel
set title "Late window" font "Helvetica,18"
plot [x2:x3] assim_file using (epoch($1)):5 with lines lc rgb c_assim lw lw_assim notitle, \
             truth_file every sub_every using (epoch($1)):5 with points lc rgb c_truth pt 7 ps ps_truth notitle

set ylabel "HRT (day)" offset 1,0
set xlabel "Simulated date" offset 0,-0.6
set xtics rotate by -30
unset title
plot [x0:x1] assim_file using (epoch($1)):10 with lines lc rgb c_assim lw lw_assim notitle, \
             truth_file every sub_every using (epoch($1)):10 with points lc rgb c_truth pt 7 ps ps_truth notitle

unset ylabel
set xlabel "Simulated date" offset 0,-0.6
plot [x2:x3] assim_file using (epoch($1)):10 with lines lc rgb c_assim lw lw_assim notitle, \
             truth_file every sub_every using (epoch($1)):10 with points lc rgb c_truth pt 7 ps ps_truth notitle

unset multiplot
unset output
