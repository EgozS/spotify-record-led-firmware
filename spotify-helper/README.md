# Spotify helper page

`index.html` is a single static page. She opens it on her phone, logs in to Spotify and
gets a code to paste into the disc's setup page. It uses only the public Client ID
(PKCE login) — no secret, no server.

## Publish it on your PERSONAL GitHub (web browser only, nothing from this PC's `gh` login)

1. Sign in to github.com with your **personal** account (use a private/incognito window so the
   work account is not involved).
2. New repository → name it e.g. `led-disc-setup` → **Public** → Create.
3. **Add file → Upload files** → drag in `index.html` from this folder → Commit.
4. **Settings → Pages** → Source: *Deploy from a branch* → Branch `main`, folder `/ (root)` → Save.
5. After a minute the page is live at `https://<your-username>.github.io/led-disc-setup/`.

## Register it with Spotify

1. https://developer.spotify.com/dashboard → your app → **Settings** → **Redirect URIs**.
2. Add exactly the URL from step 5 above, **including the trailing slash**, and Save.
   (Keep `http://127.0.0.1:8888/callback` too — the PC script uses it.)
3. **User Management** → add the email of her Spotify account (development-mode apps only work
   for listed users).

## Tell the disc about it

Edit `main/app_config.h`:

```c
#define SETUP_HELPER_URL "https://<your-username>.github.io/led-disc-setup/"
```

and reflash. The setup form then shows a link to the helper page.
