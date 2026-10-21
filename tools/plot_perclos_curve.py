"""Plot PERCLOS curve from simulated CSV data"""
import csv
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

rows = []
with open(r'D:\Fatigue-detection\output\simulated_perclos.csv') as f:
    for r in csv.DictReader(f):
        rows.append(r)

perclos = [float(r['perclos_pct']) for r in rows]
levels = [r['fatigue_level'] for r in rows]
times_s = [int(r['timestamp_ms']) / 1000 for r in rows]

fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(14, 8),
                                gridspec_kw={'height_ratios': [3, 1]})
fig.patch.set_facecolor('#1a1a2e')

ax1.set_facecolor('#16213e')
colors = {'NORMAL': '#00ff88', 'MILD': '#ffaa00', 'SEVERE': '#ff3355'}
scatter_colors = [colors[l] for l in levels]
ax1.scatter(times_s, perclos, c=scatter_colors, s=25, alpha=0.7, zorder=5)
ax1.plot(times_s, perclos, color='#4a90d9', alpha=0.3, linewidth=1, zorder=1)

ax1.axhline(y=15, color='#ffaa00', linestyle='--', alpha=0.4, linewidth=1)
ax1.axhline(y=30, color='#ff3355', linestyle='--', alpha=0.4, linewidth=1)
ax1.text(0.5, 15.5, 'MILD threshold 15%', color='#ffaa00', alpha=0.6, fontsize=9)
ax1.text(0.5, 30.5, 'SEVERE threshold 30%', color='#ff3355', alpha=0.6, fontsize=9)

stages = [
    (0, 30, 'Normal Driving', '#00ff88'),
    (30, 90, 'Fatigue Build-up', '#ffaa00'),
    (90, 120, 'Severe Fatigue', '#ff3355'),
    (120, 200, 'Rest & Recovery', '#00ccff'),
]
for s, e, label, color in stages:
    mid = (times_s[s] + times_s[min(e, len(times_s) - 1)]) / 2
    ax1.axvspan(times_s[s], times_s[min(e, len(times_s) - 1)],
                alpha=0.06, color=color)
    ax1.text(mid, 55, label, ha='center', va='bottom',
             color=color, alpha=0.8, fontsize=11, fontweight='bold')

ax1.set_ylim(-2, 62)
ax1.set_ylabel('PERCLOS (%)', color='white', fontsize=12)
ax1.set_title(
    'Simulated PERCLOS Fatigue Curve  |  Window=30 frames  |  5fps  |  40s total',
    color='white', fontsize=14, fontweight='bold')
ax1.tick_params(colors='white')
ax1.grid(alpha=0.1)

from matplotlib.lines import Line2D
legend_elements = [
    Line2D([0], [0], marker='o', color='w',
           markerfacecolor='#00ff88', markersize=8, label='NORMAL'),
    Line2D([0], [0], marker='o', color='w',
           markerfacecolor='#ffaa00', markersize=8, label='MILD'),
    Line2D([0], [0], marker='o', color='w',
           markerfacecolor='#ff3355', markersize=8, label='SEVERE'),
]
ax1.legend(handles=legend_elements, loc='upper right', labelcolor='white')

ax2.set_facecolor('#16213e')
bar_colors = [colors[l] for l in levels]
ax2.bar(times_s, [1] * len(rows), width=0.18, color=bar_colors, alpha=0.8)
ax2.set_ylim(0, 2)
ax2.set_yticks([])
ax2.set_xlabel('Time (seconds)', color='white', fontsize=12)
ax2.tick_params(colors='white')
ax2.grid(alpha=0.05)
for s, e, label, color in stages:
    mid = (times_s[s] + times_s[min(e, len(times_s) - 1)]) / 2
    ax2.text(mid, 1.5, label, ha='center',
             color=color, alpha=0.8, fontsize=10, fontweight='bold')

plt.tight_layout()
plt.savefig(r'D:\Fatigue-detection\output\perclos_curve.png',
            dpi=150, bbox_inches='tight', facecolor='#1a1a2e')
print('OK - saved to output/perclos_curve.png')
