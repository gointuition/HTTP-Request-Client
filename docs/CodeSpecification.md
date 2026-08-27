Concise, Readable, Extensible

- CamelCase
- A method should either orchestrate the workflow or implement one only specific functionality
- Every method with a return value should be assigned to a variable; avoid calling methods directly in if conditions
- If a condition is too long, extract it to a method
- Order functions from top to bottom based on the order of execution
- Fewer comments, keep key points only
- Keep logic blocks compact: declare a variable right next to its first use; group related operations together
- Report errors through one channel (basket->error); check error.code instead of return values
- Never duplicate shared logic: one implementation reused by all paths (e.g. sync/async differ by a parameter)
- isXxx functions are pure queries with no side effects; keep decisions separate from actions
- Names reflect the real action: handle (process), execute (run a full flow), prepare (set up)
- When the entry function sits on top, declare callees in a forward-declaration block, ordered by execution