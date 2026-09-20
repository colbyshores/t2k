#!/usr/bin/env python3
import argparse
import os
import sys
from urllib.parse import unquote, urljoin
from bs4 import BeautifulSoup
from curl_cffi import requests


def download_album(url_input, audio_format="flac"):
  if not url_input.startswith("http"):
    album_url = (
        f"https://downloads.khinsider.com/game-soundtracks/album/{url_input}"
    )
  else:
    album_url = url_input

  session = requests.Session(impersonate="chrome120")

  print(f"Connecting to {album_url}...")
  try:
    response = session.get(album_url)
    response.raise_for_status()
  except Exception as e:
    print(f"Error fetching album page: {e}")
    sys.exit(1)

  soup = BeautifulSoup(response.text, "html.parser")

  # Album folder naming
  album_heading = soup.find("h2")
  if album_heading:
    folder_name = "".join(
        c for c in album_heading.text if c.isalnum() or c in (" ", "_", "-")
    ).strip()
    folder_name = folder_name.replace(" ", "_")
  else:
    folder_name = album_url.rstrip("/").split("/")[-1]

  output_dir = os.path.join(os.getcwd(), folder_name)
  os.makedirs(output_dir, exist_ok=True)
  print(f"Target directory: {output_dir}\n")

  # Grab the track table
  songlist_table = soup.find("table", id="songlist")
  if not songlist_table:
    print("Error: Could not find the songlist table on this page.")
    sys.exit(1)

  intermediate_links = []

  # Target table rows directly so href structure doesn't matter
  for row in songlist_table.find_all("tr"):
    # Skip header and mass-download rows
    if row.get("id") in ["songlist_header", "songlist_footer"]:
      continue

    # Find the track link in the row
    link_td = row.find("td", class_="fullname") or row.find("td", class_="clickable-row")
    if link_td:
      a_tag = link_td.find("a")
      if a_tag and a_tag.get("href"):
        full_url = urljoin("https://downloads.khinsider.com", a_tag["href"])
        if full_url not in intermediate_links:
          intermediate_links.append(full_url)

  if not intermediate_links:
    print("No track links found in table rows. Double check the URL.")
    sys.exit(1)

  print(
      f"Found {len(intermediate_links)} tracks. Beginning harvest for"
      f" .{audio_format}...\n"
  )

  target_ext = f".{audio_format.lower()}"

  for index, page_url in enumerate(intermediate_links, 1):
    try:
      page_resp = session.get(page_url)
      page_soup = BeautifulSoup(page_resp.text, "html.parser")

      direct_audio_url = None
      fallback_url = None

      for a in page_soup.find_all("a", href=True):
        href = a["href"]
        if href.lower().endswith(target_ext):
          direct_audio_url = href
          break
        elif href.lower().endswith((".flac", ".mp3")):
          fallback_url = href

      download_url = direct_audio_url or fallback_url

      if not download_url:
        print(f"[{index}/{len(intermediate_links)}] No audio link found.")
        continue

      filename = unquote(download_url.split("/")[-1])
      filepath = os.path.join(output_dir, filename)

      if os.path.exists(filepath):
        print(
            f"[{index}/{len(intermediate_links)}] Already exists: {filename}"
        )
        continue

      if not direct_audio_url and fallback_url:
        print(
            f"[{index}/{len(intermediate_links)}] Format .{audio_format} missing,"
            f" downloading fallback: {filename}..."
        )
      else:
        print(f"[{index}/{len(intermediate_links)}] Downloading {filename}...")

      audio_resp = session.get(download_url, stream=True)
      with open(filepath, "wb") as f:
        for chunk in audio_resp.iter_content(chunk_size=16384):
          f.write(chunk)

    except Exception as err:
      print(f"[{index}/{len(intermediate_links)}] Error: {err}")

  print(f"\nFinished! Files saved to {output_dir}")


if __name__ == "__main__":
  parser = argparse.ArgumentParser(
      description="Download soundtrack albums from KHInsider."
  )
  parser.add_argument(
      "url", help="Full KHInsider album URL or album URL slug"
  )
  parser.add_argument(
      "-f",
      "--format",
      default="flac",
      choices=["flac", "mp3"],
      help="Preferred format to download (default: flac)",
  )

  args = parser.parse_args()
  download_album(args.url, args.format)
