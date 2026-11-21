# Ticker: bindings

One screen shows one symbol. All of its values start with `ticker.`. For more symbols, duplicate the screen and
rename the prefix in both the screen and its bindings, e.g. `btc.*`, `eth.*`, `spx.*`. Values are shared by every
screen on the display, so each copy needs its own prefix.

| Value | Kind | Example | Suggested source |
|---|---|---|---|
| `ticker.symbol` | text | `BTC` | a fixed text (Sample/constant), or a sensor attribute |
| `ticker.crypto` | on/off | `1` | a fixed value: `1` for a coin (shows a coin badge), leave out for a stock |
| `ticker.prefix` | text | `$` | a fixed text; leave empty for an index such as the S&P 500 |
| `ticker.price` | number | `61764.2` | `sensor.<coin>_price`, `sensor.<stock>_price` |
| `ticker.change` | number, percent | `1.25` | a 24 h change sensor or attribute (e.g. `price_change_percentage_24h`) |
| `ticker.spark` | list of numbers | 14-64 points | **History** of the price sensor, e.g. last 24 h, 48 points |

Required: `ticker.symbol` and `ticker.price`. Without `ticker.spark`, the sparkline area stays empty.

## Coin badges
When `ticker.crypto` is set, the screen draws a coin badge (`"@coin:{ticker.symbol}"`, no artwork) and moves the
symbol right to make room. BTC and ETH have their own badges; any other symbol gets a gold coin with its first
letter.

## Sources
Home Assistant has no built-in keyless stock integration. The Easy Stock integration (Home Assistant Marketplace,
Yahoo Finance, no key) works well: one sensor per symbol (`^GSPC`, `BTC-USD`, ...), state = price, attribute
`change_pct` = day change in %. Bind `ticker.price` to the state, `ticker.change` to `change_pct` and
`ticker.spark` to the state's history. Any other price sensor (REST, template) works the same way. The display never fetches anything from the
internet itself.

## Price format
- 1,000 and up: thousands separators, no cents (`$61,764`).
- Under 1,000: two decimals (`$187.42`).
- A price too wide for the row shrinks to the tiny font.
