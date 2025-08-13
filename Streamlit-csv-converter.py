import streamlit as st
import pandas as pd
import folium
import matplotlib.pyplot as plt
import matplotlib.colors as mcolors
import matplotlib.cm as cm
from streamlit_folium import st_folium
import numpy as np
import datetime
from geopy.distance import geodesic
from branca.colormap import LinearColormap
from io import BytesIO

st.title("Mini Logger V4.2 CSV Map Converter")

uploaded_file = st.file_uploader("Upload CSV file", type=["csv"])

def create_map(df):
    # Normalize speed values for colors
    norm = mcolors.Normalize(vmin=df['Speed'].min(), vmax=df['Speed'].max())
    cmap = cm.get_cmap('coolwarm')

    # Map centered on first point
    m = folium.Map(location=[df.Lat.iloc[0], df.Lng.iloc[0]], zoom_start=12)

    # Draw colored path
    for i in range(1, len(df)):
        color = mcolors.to_hex(cmap(norm(df['Speed'].iloc[i])))
        folium.PolyLine(
            locations=[(df.Lat.iloc[i-1], df.Lng.iloc[i-1]),
                       (df.Lat.iloc[i], df.Lng.iloc[i])],
            color=color,
            weight=4,
            opacity=0.8
        ).add_to(m)

    # Markers
    folium.Marker(
        location=(df.Lat.iloc[0], df.Lng.iloc[0]),
        popup="Start",
        icon=folium.Icon(color="green", icon="play")
    ).add_to(m)

    folium.Marker(
        location=(df.Lat.iloc[-1], df.Lng.iloc[-1]),
        popup="End",
        icon=folium.Icon(color="blue", icon="stop")
    ).add_to(m)

    max_speed_idx = df['Speed'].idxmax()
    popup_text = f"Fastest Point: {df['Speed'].iloc[max_speed_idx]:.1f} mph"
    if pd.api.types.is_datetime64_any_dtype(df['Time']):
        popup_text += f"<br>Time: {df['Time'].iloc[max_speed_idx]}"
    folium.Marker(
        location=(df.Lat.iloc[max_speed_idx], df.Lng.iloc[max_speed_idx]),
        popup=folium.Popup(popup_text, max_width=250),
        icon=folium.Icon(color="red", icon="flag")
    ).add_to(m)

    # Smooth legend
    colors = [mcolors.to_hex(cmap(x)) for x in np.linspace(0,1,256)]
    colormap = LinearColormap(colors=colors,
                              vmin=df['Speed'].min(),
                              vmax=df['Speed'].max(),
                              caption='Speed (MPH)')
    colormap.add_to(m)

    return m

def format_trip_duration(td):
    total_seconds = int(td.total_seconds())
    hours, remainder = divmod(total_seconds, 3600)
    minutes, seconds = divmod(remainder, 60)
    return f"{hours:02}:{minutes:02}:{seconds:02}"

if uploaded_file:
    try:
        df = pd.read_csv(uploaded_file, on_bad_lines='skip')
        # Ensure required columns exist
        required_cols = ['Lat','Lng','Speed','Time','RPM']
        if set(required_cols).issubset(df.columns):
            df = df[required_cols]
        else:
            st.error(f"CSV must contain columns: {required_cols}")
            st.stop()
    except Exception as e:
        st.error(f"Error reading CSV: {e}")
        st.stop()

    # Numeric conversion & filtering
    for col in ['Lat','Lng','Speed','RPM']:
        df[col] = pd.to_numeric(df[col], errors='coerce')
    df.dropna(subset=['Lat','Lng','Speed','RPM'], inplace=True)
    df = df[(df['Lat'].between(-90,90)) &
            (df['Lng'].between(-180,180)) &
            (df['Speed']>=0) & (df['RPM']>=0)]

    # Parse Time
    try:
        if pd.api.types.is_numeric_dtype(df['Time']):
            df['Time'] = pd.to_datetime(df['Time'], unit='s', utc=True)
        else:
            df['Time'] = pd.to_datetime(df['Time'], utc=True)
    except Exception:
        df['Time'] = pd.to_datetime(df['Time'], errors='coerce', utc=True)
    df.dropna(subset=['Time'], inplace=True)

    if df.empty:
        st.error("No valid GPS data found after cleaning.")
        st.stop()

    # Trip stats
    top_speed = df['Speed'].max()
    avg_speed = df['Speed'].mean()
    total_time = df['Time'].max() - df['Time'].min()
    total_time_str = format_trip_duration(total_time)
    total_distance_miles = sum(
        geodesic((df.Lat.iloc[i-1], df.Lng.iloc[i-1]),
                 (df.Lat.iloc[i], df.Lng.iloc[i])).miles
        for i in range(1,len(df))
    )

    st.markdown("### Summary Statistics")
    st.markdown(f"- **Top Speed:** {top_speed:.1f} mph")
    st.markdown(f"- **Average Speed:** {avg_speed:.1f} mph")
    st.markdown(f"- **Total Time:** {total_time_str}")
    st.markdown(f"- **Total Distance:** {total_distance_miles:.2f} miles")

    # Interactive map
    st.subheader("Interactive GPS Map")
    folium_map = create_map(df)
    st_folium(folium_map, width=700, height=500)

    # Plot
    st.subheader("Speed & RPM Over Time")
    fig, ax1 = plt.subplots(figsize=(10,5))
    ax1.plot(df['Time'], df['Speed'], color='tab:red', label='Speed')
    ax1.set_ylabel("Speed (MPH)", color='tab:red')
    ax1.tick_params(axis='y', labelcolor='tab:red')
    ax1.grid(True, linestyle='--', alpha=0.5)
    ax2 = ax1.twinx()
    ax2.plot(df['Time'], df['RPM'], color='tab:blue', label='RPM')
    ax2.set_ylabel("RPM", color='tab:blue')
    ax2.tick_params(axis='y', labelcolor='tab:blue')
    fig.tight_layout()
    st.pyplot(fig)

    # --- HTML download ---
    st.subheader("Download Map & Data")
    html_map = folium_map._repr_html_()  # Map HTML
    html_bytes = html_map.encode('utf-8')

    st.download_button(
        label="Download Map as HTML",
        data=html_bytes,
        file_name="mini_logger_map.html",
        mime="text/html"
    )

else:
    st.info("Upload a CSV file to get started.")