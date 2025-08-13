import streamlit as st
import pandas as pd
import folium
from streamlit_folium import st_folium
from geopy.distance import geodesic
from branca.colormap import LinearColormap
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.dates import DateFormatter

# ---------- Page setup ----------
st.set_page_config(page_title="Mini Logger CSV Converter", layout="wide")
st.title("Mini Logger CSV Map Converter")

# ---------- Helpers ----------
def normalize_columns(df: pd.DataFrame) -> pd.DataFrame:
    """Rename columns to standard: Lat, Lng, Speed, Time, RPM (if present)."""
    col_map = {c: c for c in df.columns}
    lc = {c: c.lower().replace(" ", "").replace("_", "") for c in df.columns}

    def find(syns):
        for orig, key in lc.items():
            if key in syns:
                return orig
        return None

    lat_col = find({"lat", "latitude"})
    lng_col = find({"lng", "lon", "longitude"})
    spd_col = find({"speed", "speedmph", "speed_mph", "mph"})
    time_col = find({"time", "utcdatetime", "datetime", "timestamp"})
    rpm_col = find({"rpm"})

    renames = {}
    if lat_col: renames[lat_col] = "Lat"
    if lng_col: renames[lng_col] = "Lng"
    if spd_col: renames[spd_col] = "Speed"
    if time_col: renames[time_col] = "Time"
    if rpm_col: renames[rpm_col] = "RPM"

    df = df.rename(columns=renames)
    return df

def parse_time_series(s: pd.Series) -> pd.Series:
    """
    Parse Time column robustly:
    - If numeric, auto-detect seconds vs milliseconds since epoch.
    - If string/datetime, parse with utc=True, then strip tz to make Matplotlib happy.
    Returns tz-naive pandas datetime64[ns].
    """
    if np.issubdtype(s.dtype, np.number):
        # Heuristic: ms since epoch if values are large
        vmax = s.dropna().astype(float).max()
        unit = "ms" if vmax > 1e11 else "s"
        ts = pd.to_datetime(s, unit=unit, utc=True, errors="coerce")
    else:
        # Try flexible string parsing
        ts = pd.to_datetime(s, utc=True, errors="coerce")

    # Strip timezone (keep UTC clock values but make tz-naive for Matplotlib)
    return ts.dt.tz_convert(None) if ts.dt.tz is not None else ts.dt.tz_localize(None)

def clean_dataframe(df: pd.DataFrame) -> pd.DataFrame:
    """Coerce numeric cols, drop invalid rows, sort by time."""
    for col in ["Lat", "Lng", "Speed", "RPM"]:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")

    # Drop rows with missing essentials
    req = ["Lat", "Lng", "Time"]
    for r in req:
        if r not in df.columns:
            st.error(f"Missing required column: **{r}**")
            st.stop()

    df = df.dropna(subset=req)

    # Keep sane values
    df = df[
        df["Lat"].between(-90, 90) &
        df["Lng"].between(-180, 180)
    ]
    if "Speed" in df.columns:
        df = df[df["Speed"] >= 0]
    if "RPM" in df.columns:
        df = df[df["RPM"] >= 0]

    # Sort by time
    df = df.sort_values("Time").reset_index(drop=True)
    return df

def create_speed_colormap(min_speed, max_speed):
    # 3-stop gradient, works nicely for legends
    return LinearColormap(
        colors=["#2c7bb6", "#ffff8c", "#d7191c"],  # blue -> yellow -> red
        vmin=float(min_speed),
        vmax=float(max_speed)
    )

def draw_map(df: pd.DataFrame) -> folium.Map:
    # Center map on first valid point
    m = folium.Map(location=[df["Lat"].iloc[0], df["Lng"].iloc[0]], zoom_start=12, control_scale=True)

    # Draw colored path by Speed if present
    if "Speed" in df.columns and not df["Speed"].isna().all():
        min_s, max_s = float(df["Speed"].min()), float(df["Speed"].max())
        cmap = create_speed_colormap(min_s, max_s)

        for i in range(1, len(df)):
            color = cmap(df["Speed"].iloc[i]) if pd.notna(df["Speed"].iloc[i]) else "#3388ff"
            folium.PolyLine(
                locations=[(df["Lat"].iloc[i-1], df["Lng"].iloc[i-1]),
                           (df["Lat"].iloc[i], df["Lng"].iloc[i])],
                color=color,
                weight=4,
                opacity=0.9
            ).add_to(m)

        cmap.caption = "Speed (MPH)"
        cmap.add_to(m)
    else:
        # Plain line if no speed
        folium.PolyLine(
            locations=list(zip(df["Lat"], df["Lng"])),
            weight=4,
            opacity=0.9
        ).add_to(m)

    # Start marker
    folium.Marker(
        location=(df["Lat"].iloc[0], df["Lng"].iloc[0]),
        popup="Start",
        icon=folium.Icon(color="green", icon="play")
    ).add_to(m)

    # End marker
    folium.Marker(
        location=(df["Lat"].iloc[-1], df["Lng"].iloc[-1]),
        popup="End",
        icon=folium.Icon(color="blue", icon="stop")
    ).add_to(m)

    # Fastest point marker (if Speed exists)
    if "Speed" in df.columns and not df["Speed"].isna().all():
        max_idx = int(df["Speed"].idxmax())
        popup_text = f"Fastest: {df['Speed'].iloc[max_idx]:.1f} mph"
        if pd.api.types.is_datetime64_any_dtype(df["Time"]):
            popup_text += f"<br>Time: {df['Time'].iloc[max_idx]}"
        folium.Marker(
            location=(df["Lat"].iloc[max_idx], df["Lng"].iloc[max_idx]),
            popup=folium.Popup(popup_text, max_width=260),
            icon=folium.Icon(color="red", icon="flag")
        ).add_to(m)

    return m

def summarize_trip(df: pd.DataFrame):
    # Distance in miles
    total_miles = 0.0
    lat = df["Lat"].to_numpy()
    lng = df["Lng"].to_numpy()
    for i in range(1, len(df)):
        total_miles += geodesic((lat[i-1], lng[i-1]), (lat[i], lng[i])).miles

    # Duration
    start_t = df["Time"].iloc[0]
    end_t = df["Time"].iloc[-1]
    duration = end_t - start_t
    # Make a clean H:MM:SS string
    duration_str = str(pd.to_timedelta(duration))

    # Speeds
    top_speed = float(df["Speed"].max()) if "Speed" in df.columns else None
    avg_speed = float(df["Speed"].mean()) if "Speed" in df.columns else None

    return {
        "start": start_t,
        "end": end_t,
        "duration": duration,
        "duration_str": duration_str,
        "distance_mi": total_miles,
        "top_speed": top_speed,
        "avg_speed": avg_speed
    }

def plot_speed_rpm(df: pd.DataFrame):
    x = df["Time"]
    fig, ax1 = plt.subplots(figsize=(10, 5))

    # Speed
    if "Speed" in df.columns:
        ax1.plot(x, df["Speed"], label="Speed", color="tab:red")
        ax1.set_ylabel("Speed (MPH)", color="tab:red")
        ax1.tick_params(axis="y", labelcolor="tab:red")

    # RPM (secondary axis)
    if "RPM" in df.columns:
        ax2 = ax1.twinx()
        ax2.plot(x, df["RPM"], label="RPM", color="tab:blue")
        ax2.set_ylabel("RPM", color="tab:blue")
        ax2.tick_params(axis="y", labelcolor="tab:blue")

    ax1.set_xlabel("Time (UTC)")
    ax1.xaxis.set_major_formatter(DateFormatter("%Y-%m-%d\n%H:%M:%S"))
    fig.tight_layout()
    return fig

# ---------- UI ----------
uploaded = st.file_uploader("Upload CSV file", type=["csv"])

if uploaded:
    # Try read with header; if user exported headerless, try fallback
    try:
        df_raw = pd.read_csv(uploaded)
    except Exception:
        uploaded.seek(0)
        df_raw = pd.read_csv(uploaded, header=None)
        # If headerless, assign the expected columns (best-effort)
        df_raw.columns = ["Lat", "Lng", "Speed", "Time", "RPM"][: df_raw.shape[1]]

    df = normalize_columns(df_raw.copy())

    # Ensure Time column exists after normalization
    if "Time" not in df.columns:
        st.error("Could not detect a time column (e.g., Time / UTC_datetime / timestamp).")
        st.stop()

    # Parse time robustly and clean
    df["Time"] = parse_time_series(df["Time"])
    df = clean_dataframe(df)

    if df.empty:
        st.error("No valid GPS rows after cleaning.")
        st.stop()

    # Show summary
    stats = summarize_trip(df)

    st.subheader("Summary Statistics")
    c1, c2, c3, c4 = st.columns(4)
    with c1:
        st.metric("Top Speed", f"{stats['top_speed']:.1f} mph" if stats["top_speed"] is not None else "—")
    with c2:
        st.metric("Average Speed", f"{stats['avg_speed']:.1f} mph" if stats["avg_speed"] is not None else "—")
    with c3:
        st.metric("Total Distance", f"{stats['distance_mi']:.2f} mi")
    with c4:
        st.metric("Trip Time", stats["duration_str"])

    st.caption(f"Start: {stats['start']}  |  End: {stats['end']} (UTC)")

    # Map
    st.subheader("Interactive GPS Map")
    m = draw_map(df)
    st_folium(m, width=900, height=540)

    # Charts
    st.subheader("Speed & RPM Over Time")
    fig = plot_speed_rpm(df)
    st.pyplot(fig)

else:
    st.info("Upload a CSV file to get started.")
