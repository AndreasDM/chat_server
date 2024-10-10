# Chat Server

chat server using boost asio & beast

# Prerequisites

* Boost
* Fmt

# Run

```
make run
```

# Notes

On linux you would probably have to open the selected port

```
sudo ufw allow 12345/tcp
```

and delete the rule after testing

```
sudo ufw delete allow 12345/tcp
```

----

The hardcoded `localhost` in the html body needs to be
changed for it to work.
