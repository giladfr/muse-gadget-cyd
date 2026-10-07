---
name: gadget-cyd-desk-dashboard
description: >-
  Use the user's CYD desk dashboard (ESP32-2432S028R, 320x240 touch screen, a paired Muse
  gadget) proactively: show your own cards with buttons the user can tap to answer you, post
  notification banners, push weather and calendar data, show images, and collect taps. Use
  when the dashboard is paired and the user would benefit from glanceable information or a
  one-tap decision at their desk.
---

# CYD Desk Dashboard

A small always-on touch screen on the user's desk, paired to you as a gadget. It shows live
stock quotes (it fetches them itself), weather and today's calendar, and up to four screens of
your own. Taps on your buttons come back to you as chat messages starting with
`[Desk display]`.

## Identify the Device

- It advertises the `dashboard.*` commands (`dashboard.card`, `dashboard.notify`,
  `dashboard.data`, `dashboard.events`, `dashboard.stocks`). Use those, not raw display
  commands, unless you need a full-screen image.
- Check `dashboard.debug` if something looks wrong: it reports touch, backlight, live-quote
  status (`http=`), queued events and free memory.

## Workflow

### Ask for a decision (approval card)

Use this before acting on the user's behalf when a tap is enough to decide.

**Important:** the `json` parameter must be a JSON *string*, not an object.
Stringify the card object first.

```json
dashboard.card {"id": "groceries", "json": "{\"id\":\"groceries\",\"title\":\"Groceries\",\"sub\":\"Tuesday order\",\"tone\":\"accent\",\"text\":\"Your usual order is ready: milk, eggs, sourdough, bananas and coffee. Place it for tomorrow 9-11 AM?\",\"rows\":[{\"label\":\"Total\",\"value\":\"$64.20\",\"detail\":\"5 items\"}],\"buttons\":[{\"id\":\"yes\",\"label\":\"Order\",\"say\":\"Yes, place the usual grocery order for tomorrow 9-11.\"},{\"id\":\"later\",\"label\":\"Later\",\"say\":\"Remind me about groceries tonight.\"},{\"id\":\"no\",\"label\":\"Skip\",\"say\":\"Skip groceries this week.\"}],\"show\":true,\"ttl_s\":14400}"}
```

- Write each button's `say` as the instruction you want to receive: the tap arrives as
  `[Desk display] <say>`. Without `say` you get a description of the tap.
- When the answer arrives, act on it, then replace the card (same `id`) with the outcome or
  remove it: `dashboard.card {"id": "groceries", "remove": true}`.
- If a decision is not urgent, leave `show` off: the card joins the screen rotation.
- If taps don't seem to reach you, collect them: `dashboard.events` (the board queues the
  last 8; `peek: true` leaves them queued).

### Show status (rows card)

```json
dashboard.card {"id": "day", "json": "{\"id\":\"day\",\"title\":\"Your day\",\"sub\":\"so far\",\"rows\":[{\"label\":\"Steps\",\"value\":\"6,240\",\"detail\":\"goal 10,000\",\"progress\":62,\"tone\":\"up\"},{\"label\":\"Inbox\",\"value\":\"14\",\"detail\":\"3 need a reply\",\"tone\":\"accent\",\"spark\":[22,19,25,30,18,16,14]},{\"label\":\"Build\",\"value\":\"failing\",\"detail\":\"main, 12 min ago\",\"tone\":\"down\"}]}"}
```

Rows: up to 5; `tone` is `up` (green), `down` (red), `accent` (amber), `blue` or `dim`;
`progress` 0-100 draws a bar; `spark` (any numbers) draws a sparkline. Update the same `id` as
things change.

### Notify (banner)

```json
dashboard.notify {"text": "Package delivered", "detail": "Front porch, 10:41 AM", "level": "success"}
```

`level`: `info`, `success`, `warning`, `alert`. Shows for 20 s (`ttl_s`; 0 = until tapped) and
wakes a dimmed screen. `"card": "<id>"` makes a tap open that card. Up to 4 queue; when full,
the oldest waiting banner is dropped. No history — once a banner expires or is tapped, it's gone.
For persistent alerts, use a card instead.

### Data the built-in screens use

- Weather: run `bridge/fetch.py` from the gadget's repo and push its output with
  `dashboard.data {"screen": "bridge", "json": "<output as a JSON string>"}`. Stocks in it are ignored while the
  board fetches its own quotes.
- Calendar: `dashboard.data {"screen": "calendar", "json": "{\"label\":\"Tuesday, Oct 6\",\"events\":[{\"time\":\"5:45 PM\",\"title\":\"...\"}]}"}`.
  Push in the morning and when it changes. The `json` parameter must be a string.
- Watchlist: `dashboard.stocks {"symbols": "AMD,NVDA,SPY"}` (up to 8).

### Images

`dashboard.takeover {"url": "http://..."}` shows a baseline JPEG full-screen with an X (back to
the dashboard after 10 minutes). For files you will show again, put them on the board's SD
card once with `sd.fetch {"url", "path"}` and show them with `display.draw_sd {"path"}`.

## Good proactive uses

- Approvals before you act: orders, bookings, replies, purchases, calendar changes.
- Meeting heads-up 10 minutes before: a card with the title, time, attendees and a
  "Join"/"Running late" choice whose `say` tells you what to do.
- Morning card: first meeting, weather headline, what needs attention today.
- Watchers: build status, package tracking, price thresholds on the user's stocks; update a
  rows card and use a banner only when something changes.
- End of day: a short recap card; remove stale cards so the rotation stays useful.

## Verify the Result

- Commands answer `{"ok": true}`; an error names the bad field.
- After a tap you get the `[Desk display]` message; the card shows "Sent to Muse" under its
  title.

## Limits

- 320x240 screen: keep titles to ~16 characters, text to ~4 short lines, button labels to one
  word or two.
- At most 4 cards. Cards and banners live in RAM: after a reboot, push them again.
- No microphone or camera. The board only sends taps; it doesn't show your chat replies, so
  answer through the app or by updating the card.
- Use banners sparingly; don't notify at night unless it is urgent.
