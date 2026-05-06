# Security Policy

Report security issues privately to the repository owner. Do not open public
issues for vulnerabilities.

`uproxy` treats all network, config, and CLI input as hostile. The parser and
proxy layers enforce explicit limits for header count, header size, frame size,
connection pool capacity, and HTTP/2 flow-control windows.

