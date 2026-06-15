# =============================================================================
# plot_paper_wetland_hrt_full_period.gp
# Single full-simulation-period HRT comparison for wetland paper.
# Uses normalized CSVs prepared by prepare_wetland_plot_inputs.py.
# =============================================================================

set datafile separator ","
set datafile missing "nan"

truth_file = "Wetland_assimilation/outputs/paper_plot_inputs/truth_normalized.csv"
assim_file = "Wetland_assimilation/outputs/paper_plot_inputs/assim_normalized.csv"
outfile    = "paper_wetland_hrt_full_period.png"

set terminal pngcairo size 1600,650 enhanced font "Helvetica,18"
set output outfile

epoch(d) = (d - 25569.0) * 86400.0
set xdata time
set timefmt "%s"
set format x "%Y-%m"

set grid xtics ytics lc rgb "#cccccc" lw 0.8
set tics nomirror
set border 3
set xtics rotate by -30

c_truth = "#d62728"
c_assim = "#1f77b4"
ps_truth = 0.45
lw_assim = 2.2
sub_every = 5

set title "Hydraulic residence time over the full simulation period" font "Helvetica,20"
set xlabel "Simulated date" offset 0,-0.6
set ylabel "HRT (day)" offset 1,0
set key top right inside box opaque samplen 2 spacing 1.2 font "Helvetica,16" width 0

plot assim_file using (epoch($1)):10 with lines lc rgb c_assim lw lw_assim title "Assimilation model", \
     truth_file every sub_every using (epoch($1)):10 with points lc rgb c_truth pt 7 ps ps_truth title "Truth"

unset output
