import streamlit as st
import pandas as pd
import folium
import matplotlib.pyplot as plt
import matplotlib.colors as mcolors
import matplotlib.cm as cm
from streamlit_folium import st_folium
import datetime
from geopy.distance import geodesic
from branca.colormap import LinearColormap

st.title("Mini Logger V4.1 CSV Map Converter")

uploaded_file = st.file_uploader("Upload CSV file", type=["csv"])

def create_map(df):
    # Normalize speed values for colors
    norm = mcolors.Normalize(vmin=df['Speed'].min(), vmax=df['Speed'].max())
    cmap = cm.get_cmap('coolwarm')

    # Create Folium map
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

    # Add start pin
    folium.Marker(
        location=(df.Lat.iloc[0], df.Lng.iloc[0]),
        popup="Start",
        icon=folium.Icon(color="green", icon="play")
    ).add_to(m)

    # Add end pin
    folium.Marker(
        location=(df.Lat.iloc[-1], df.Lng.iloc[-1]),
        popup="End",
        icon=folium.Icon(color="blue", icon="stop")
    ).add_to(m)

    # Add fastest speed pin
    max_speed_idx = df['Speed'].idxmax()
    popup_text = f"Fastest Point: {df['Speed'].iloc[max_speed_idx]:.1f} mph"
    if pd.api.types.is_datetime64_any_dtype(df['Time']):
        popup_text += f"<br>Time: {df['Time'].iloc[max_speed_idx]}"
    folium.Marker(
        location=(df.Lat.iloc[max_speed_idx], df.Lng.iloc[max_speed_idx]),
        popup=folium.Popup(popup_text, max_width=250),
        icon=folium.Icon(color="red", icon="flag")
    ).add_to(m)

    # Add legend for speed
    colormap = LinearColormap(
        colors=[mcolors.to_hex(cmap(0)), mcolors.to_hex(cmap(0.5)), mcolors.to_hex(cmap(1))],
        vmin=df['Speed'].min(),
        vmax=df['Speed'].max(),
        caption='Speed (MPH)'
    )
    colormap.add_to(m)

    return m

if uploaded_file:
